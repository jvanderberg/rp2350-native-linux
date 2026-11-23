// SPDX-License-Identifier: GPL-2.0
/*
 * RP2350 per-process address spaces through QMI address translation.
 * See docs/qmi-atrans.md for the hardware behaviour this relies on,
 * and docs/atrans.md for the model and the ABI ("ATRANS v2").
 *
 * An "ATRANS process" is a static ELF whose PT_LOADs all lie in the 4 MiB
 * window at ATRANS_WINDOW (0x11000000). It owns one physically contiguous
 * PSRAM block, shown at the window while it runs (ATRANS4: BASE = the
 * block, SIZE = its length, so the block always covers [window, window +
 * size)). Window layout:
 *
 *   0x11000000 .. stack_top       stack, grows down (stack_top = the lowest
 *                                 PT_LOAD p_vaddr, 0x11020000 by default)
 *   stack_top .. start_brk        image (text, data, bss)
 *   start_brk .. brk              heap
 *   .. block top                  mmap pages, highest free run first
 *   block top .. 0x11400000       not backed: MemManage (MPU) -> SIGSEGV
 *
 * The block grows on demand (brk, mmap) up to the whole window: a bigger
 * block is allocated, what is in use is copied into it, ATRANS4 is
 * repointed and the old block is freed. Window addresses never change.
 * It shrinks in place when a good part of its top is free again.
 *
 * - switch_mm: when a different ATRANS process runs, clean + invalidate the
 *   XIP cache (one pass, cleans issued at the top of the maintenance
 *   window: a low-address set/way clean rewrites line tags) and point
 *   ATRANS4 and MPU region 0 at its block. Other processes leave the window
 *   alone and run with the MPU off.
 * - fork: a new block for the child, filled with what the parent uses (the
 *   stack from sp up, image to brk, mmapped pages) by copying the window
 *   (the parent is current) to the child's block through the uncached,
 *   untranslated alias.
 * - exit: write back (if loaded), unmap, free.
 *
 * Cache rules (the XIP cache is virtually tagged, write-back): lines tagged
 * with window addresses only ever belong to the loaded block (every ATRANS4
 * change is preceded by a full clean + invalidate), so an unloaded block can
 * be read and written through the uncached alias. A newly allocated block
 * may still have dirty lines under its kernel address (kva) from its last
 * kernel use: the cache is flushed before the block is written any other
 * way. The kernel never touches a block through its kva.
 *
 * Kernel RAM is the same PSRAM through ATRANS5/6 at CONFIG_DRAM_BASE (boot
 * stub: device offset 0 there), so a block's device offset is
 * kva - CONFIG_DRAM_BASE.
 */
#include <linux/binfmts.h>
#include <linux/cred.h>
#include <linux/elf.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/moduleparam.h>
#include <linux/personality.h>
#include <linux/proc_fs.h>
#include <linux/random.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task_stack.h>
#include <linux/swap.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <asm/mmu_context.h>
#include <asm/rp2350-atrans.h>
#include <asm/thread_notify.h>
#include "rp2350-atrans-internal.h"

static LIST_HEAD(texts);
static DEFINE_MUTEX(text_lock);

unsigned long loaded;			/* kva of the block ATRANS4 maps, 0: none */
static unsigned long loaded_text;	/* kva of the text block ATRANS7 maps */

static inline unsigned long mm_tsize(struct mm_struct *mm)
{
	struct rp2350_atrans_mm *a = mm ? mm->context.atrans : NULL;

	return a && a->text ? a->text->size : 0;
}

static struct {
	unsigned long switches, switch_cycles, switch_max;
	unsigned long forks, fork_cycles, fork_max, fork_bytes;
	unsigned long grows, grow_cycles, grow_max, grow_bytes, shrinks, grows_inplace;
	unsigned long execs, exits, live, live_bytes, nomem, drops, reserve, gaveup, lowres;
	unsigned long mmaps, munmaps, badsp;
	unsigned long texts, text_bytes, text_loads, text_hits;
} st;

static inline u32 cycles(void)
{
	return readl_relaxed(DWT_CYCCNT);
}

/* Clean (high address) and invalidate every set/way. Interrupts off. */
void xip_flush_all(void)
{
	unsigned long o;

	for (o = 0; o < XIP_CACHE; o += XIP_LINE) {
		writeb_relaxed(0, (void __iomem *)(XIP_MAINT + XIP_SETWAY_HI + o + 1));
		writeb_relaxed(0, (void __iomem *)(XIP_MAINT + o));
	}
	mb();
}

/*
 * M33 MPU (PMSAv8, Secure instance: nothing here switches to Non-secure).
 * While an ATRANS process is current the MPU is on with PRIVDEFENA, so the
 * kernel keeps the default map, and user mode reaches only:
 *   region 0: [window + 32, window + block size), read/write
 *   region 1: [window, window + 32), read-only: the thread pointer word
 *   region 2: [text window, + text block size), read-only and executable
 *             (v3 programs; disabled for v2 ones)
 * Everything else (kernel RAM, MMIO, SRAM, flash, the window past the
 * block) faults as MemManage -> SIGSEGV. Other processes run with the MPU
 * off, as stock nommu. The regions are non-shareable like the default
 * map's Code region, so LDREX/STREX keep using the local monitor (PSRAM has
 * no global one). PMSAv8 has no "kernel RW, user RO" encoding, so the
 * kernel writes the thread pointer word with the MPU briefly off.
 */
#define M33_MPU_CTRL	((void __iomem *)0xe000ed94)
#define M33_MPU_RNR	((void __iomem *)0xe000ed98)
#define M33_MPU_RBAR	((void __iomem *)0xe000ed9c)
#define M33_MPU_RLAR	((void __iomem *)0xe000eda0)
#define M33_MPU_MAIR0	((void __iomem *)0xe000edc0)
#define M33_MPU_ENABLE	BIT(0)
#define M33_MPU_PRIVDEFENA	BIT(2)
#define RBAR_AP_RW_ANY	(1 << 1)	/* SH 0, XN 0 */
#define RBAR_AP_RO_ANY	(3 << 1)
#define RLAR_EN		BIT(0)		/* AttrIndx 0 */
#define MAIR_NORMAL_WT	0xaa		/* as the default map's Code region */

static unsigned long mpu_size;		/* size of the window the MPU allows, 0: MPU off */
static unsigned long mpu_tsize;		/* size of the text region allowed */

/* MPU for mm (NULL or not ATRANS: off). Interrupts off. */
void mpu_confine(struct mm_struct *mm)
{
	unsigned long size = mm && mm->context.atrans_kva ? mm->context.atrans_size : 0;
	unsigned long tsize = size ? mm_tsize(mm) : 0;

	rp2350_atrans_dev_mpu(size ? mm : NULL);	/* device ranges, regions 5-7 */
	if (size == mpu_size && tsize == mpu_tsize)
		return;
	writel_relaxed(0, M33_MPU_CTRL);
	if (size) {
		writel_relaxed(0, M33_MPU_RNR);
		writel_relaxed((ATRANS_WINDOW + ATRANS_TPSIZE) | RBAR_AP_RW_ANY, M33_MPU_RBAR);
		writel_relaxed(((ATRANS_WINDOW + size - 1) & ~0x1fUL) | RLAR_EN, M33_MPU_RLAR);
		writel_relaxed(2, M33_MPU_RNR);
		writel_relaxed(ATRANS_TEXT | RBAR_AP_RO_ANY, M33_MPU_RBAR);
		writel_relaxed(tsize ? ((ATRANS_TEXT + tsize - 1) & ~0x1fUL) | RLAR_EN : 0, M33_MPU_RLAR);
		writel_relaxed(M33_MPU_PRIVDEFENA | M33_MPU_ENABLE, M33_MPU_CTRL);
	}
	dsb(sy);
	isb();
	mpu_size = size;
	mpu_tsize = tsize;
}

/* Point ATRANS4 (and, for the current process, the MPU) at a block. The
 * caller has flushed the XIP cache; interrupts off. */
void atrans_map(unsigned long kva, unsigned long size)
{
	u32 v = kva ? (size >> 12) << 16 | ((kva - CONFIG_DRAM_BASE) >> 12) : 0;

	writel_relaxed(v, QMI_ATRANS4);
	readl_relaxed(QMI_ATRANS4);
	loaded = kva;
}

/*
 * Point ATRANS7 at mm's text block (or nothing). Only right after a full
 * cache flush, which always comes with a data window change: text lines are
 * never dirty, but lines of the previous text must not hit. Interrupts off.
 */
void text_map(struct mm_struct *mm)
{
	struct rp2350_atrans_mm *a = mm ? mm->context.atrans : NULL;
	struct atrans_text *t = a ? a->text : NULL;
	u32 v = t ? (t->size >> 12) << 16 | ((t->kva - CONFIG_DRAM_BASE) >> 12) : 0;

	if ((t ? t->kva : 0) == loaded_text)
		return;
	writel_relaxed(v, QMI_ATRANS7);
	readl_relaxed(QMI_ATRANS7);
	loaded_text = t ? t->kva : 0;
}

/*
 * Thread pointer fast path: the current thread's TLS pointer (set_tls,
 * CLONE_SETTLS) is kept in the word at ATRANS_TP, which user mode can read
 * but not write, so __aeabi_read_tp can be "mov r0, #0x11000000; ldr r0,
 * [r0]". It is rewritten whenever an ATRANS thread is switched in and on
 * set_tls. The get_tls syscall keeps working. Interrupts off, the window
 * shows current's block.
 */
void tp_store(unsigned long tp)
{
	if (mpu_size) {
		writel_relaxed(0, M33_MPU_CTRL);
		dsb(sy);
		isb();
	}
	*(volatile unsigned long *)ATRANS_TP = tp;
	if (mpu_size) {
		writel_relaxed(M33_MPU_PRIVDEFENA | M33_MPU_ENABLE, M33_MPU_CTRL);
		dsb(sy);
		isb();
	}
}

void rp2350_atrans_set_tls(unsigned long tp)
{
	unsigned long flags;

	if (!current->mm || !current->mm->context.atrans_kva)
		return;
	local_irq_save(flags);
	tp_store(tp);
	local_irq_restore(flags);
}

/*
 * A fault while stacking onto a bad user sp (mach-rp2350/fault.c): the task
 * is killed, but the exception return may still pop a frame from PSP before
 * PendSV gets to run, and with the MPU on, user mode can only read the
 * window: a frame in kernel memory then faults again (MUNSTKERR), forever.
 * So put a harmless frame in the read-only granule at ATRANS_TP (the thread
 * pointer stays in its r0 slot) and return its address for PSP; 0 when the
 * MPU is off (then any frame address is readable). Interrupts off.
 */
unsigned long rp2350_atrans_stkerr_frame(void)
{
	volatile unsigned long *f = (volatile unsigned long *)ATRANS_TP;
	struct mm_struct *mm = current->mm;
	int i;

	if (!mpu_size || !mm || !loaded || mm->context.atrans_kva != loaded)
		return 0;
	writel_relaxed(0, M33_MPU_CTRL);
	dsb(sy);
	isb();
	for (i = 1; i < 7; i++)
		f[i] = 0;
	f[7] = 0x01000000;		/* xPSR: Thumb */
	writel_relaxed(M33_MPU_PRIVDEFENA | M33_MPU_ENABLE, M33_MPU_CTRL);
	dsb(sy);
	isb();
	return ATRANS_TP;
}

static int atrans_thread_notify(struct notifier_block *nb, unsigned long cmd, void *t)
{
	struct thread_info *ti = t;
	struct mm_struct *mm;

	if (cmd != THREAD_NOTIFY_SWITCH)
		return NOTIFY_DONE;
	/* THREAD_INFO_IN_TASK; switch_mm has already loaded the block */
	mm = container_of(ti, struct task_struct, thread_info)->mm;
	if (mm && mm->context.atrans_kva && mm->context.atrans_kva == loaded)
		tp_store(ti->tp_value[0]);
	return NOTIFY_DONE;
}

static struct notifier_block atrans_thread_nb = {
	.notifier_call = atrans_thread_notify,
};

void rp2350_atrans_switch(struct mm_struct *next)
{
	unsigned long flags, kva = next ? next->context.atrans_kva : 0;
	u32 c0, c;

	local_irq_save(flags);
	if (!kva) {
		if (next && next->context.atrans && loaded) {
			/* swapped out: it swaps in at the end of schedule(),
			 * before touching the window; show nothing meanwhile */
			xip_flush_all();
			atrans_map(0, 0);
		}
		mpu_confine(NULL);
		local_irq_restore(flags);
		return;
	}
	if (kva != loaded) {
		/* one flush covers both windows; ATRANS7 is only rewritten
		 * when the program differs */
		c0 = cycles();
		xip_flush_all();
		atrans_map(kva, next->context.atrans_size);
		text_map(next);
		c = cycles() - c0;
		st.switches++;
		st.switch_cycles += c;
		if (c > st.switch_max)
			st.switch_max = c;
	}
	mpu_confine(next);
	local_irq_restore(flags);
}

/*
 * __invalid_entry (entry-v7m.S, patched by apply.sh): a HardFault, or a
 * fault escalated to one because it hit with interrupts off. The fault
 * status, and the windows against what current expects.
 */
void rp2350_atrans_fault_dump(void)
{
	struct mm_struct *mm = current->mm;

	pr_emerg("fault: CFSR %08x HFSR %08x MMFAR %08x BFAR %08x\n",
		 readl_relaxed((void __iomem *)0xe000ed28), readl_relaxed((void __iomem *)0xe000ed2c),
		 readl_relaxed((void __iomem *)0xe000ed34), readl_relaxed((void __iomem *)0xe000ed38));
	pr_emerg("atrans: ATRANS4 %08x ATRANS7 %08x loaded %08lx text %08lx mpu %lu/%lu KiB\n",
		 readl_relaxed(QMI_ATRANS4), readl_relaxed(QMI_ATRANS7), loaded, loaded_text,
		 mpu_size >> 10, mpu_tsize >> 10);
	if (mm && mm->context.atrans)
		pr_emerg("atrans: %s[%d] block %08lx %lu KiB, text %08lx, swap %s\n",
			 current->comm, task_pid_nr(current), mm->context.atrans_kva,
			 mm->context.atrans_size >> 10,
			 mm->context.atrans->text ? mm->context.atrans->text->kva : 0,
			 mm->context.atrans->swap ? "yes" : "no");
}

/*
 * access_ok() (arch/arm/include/asm/uaccess.h, patched by apply.sh): an
 * ATRANS process's user pointers must lie inside its block (above the
 * thread pointer word). Stock nommu
 * accepts any address, so a syscall could read or write kernel memory.
 */
bool rp2350_atrans_access_ok(unsigned long addr, unsigned long size)
{
	struct mm_struct *mm = current->mm;
	unsigned long win;

	if (!mm || !mm->context.atrans)
		return size <= TASK_SIZE && addr <= TASK_SIZE - size;	/* stock */
	/* zero bytes touch nothing: write(fd, NULL, 0) and musl's {NULL, 0}
	 * second iovec must succeed, as on stock Linux */
	if (!size)
		return true;
	/* not the thread pointer word: user mode may not write it, and a
	 * privileged write there faults (MPU region 1 is read-only) */
	win = mm->context.atrans_size;
	if (addr >= ATRANS_WINDOW + ATRANS_TPSIZE && size <= win &&
	    addr - ATRANS_WINDOW <= win - size)
		return true;
	/* the shared text, for reading: the write paths (put_user,
	 * copy_to_user, clear_user, futex ops) refuse it, patched in
	 * asm/uaccess.h and asm/futex.h (rp2350_atrans_ro) */
	win = mm_tsize(mm);
	return win && addr >= ATRANS_TEXT && size <= win && addr - ATRANS_TEXT <= win - size;
}
EXPORT_SYMBOL(rp2350_atrans_access_ok);

/* TASK_SIZE_MAX: strnlen_user() and strncpy_from_user() check
 * access_ok(str, TASK_SIZE_MAX - str), so it is the block's end. */
unsigned long rp2350_atrans_task_size_max(void)
{
	struct mm_struct *mm = current->mm;

	if (!mm || !mm->context.atrans)
		return TASK_SIZE;
	return ATRANS_WINDOW + mm->context.atrans_size;
}
EXPORT_SYMBOL(rp2350_atrans_task_size_max);

/* strnlen_user()/strncpy_from_user() (lib/, patched by apply.sh): the end
 * of the region holding the string, which may be in the text window */
unsigned long rp2350_atrans_str_max(unsigned long addr)
{
	struct mm_struct *mm = current->mm;
	unsigned long tsize = mm_tsize(mm);

	if (tsize && addr >= ATRANS_TEXT && addr < ATRANS_TEXT + tsize)
		return ATRANS_TEXT + tsize;
	return rp2350_atrans_task_size_max();
}
EXPORT_SYMBOL(rp2350_atrans_str_max);

/*
 * A user stack pointer the kernel will write an exception frame below (on
 * every return to user mode, with privileged stores): for an ATRANS process
 * it must leave room for the frame inside the block. Checked on sigreturn
 * (valid_user_regs) and for clone's new stack (copy_thread).
 */
int rp2350_atrans_sp_ok(struct mm_struct *mm, unsigned long sp)
{
	if (!mm || !mm->context.atrans)
		return 1;
	return sp >= ATRANS_WINDOW + ATRANS_TPSIZE + 40 &&
	       sp <= ATRANS_WINDOW + mm->context.atrans_size;
}

int rp2350_atrans_user_regs_ok(struct pt_regs *regs)
{
	if (rp2350_atrans_sp_ok(current->mm, regs->ARM_sp))
		return 1;
	st.badsp++;
	return 0;
}

/* ---- blocks ------------------------------------------------------------------ */

/*
 * Physically contiguous runs of any length. alloc_pages_exact() needs a
 * free buddy block of the next power of two (a 2.1 MiB block needs an
 * aligned, entirely free 4 MiB: with 8 MiB of RAM, at most one such
 * block ever exists). Instead, scan the zone for runs of free buddy
 * blocks, take the smallest run that fits (best fit), pull its blocks off
 * the free lists and give back what lies past the end. The same steps as
 * alloc_contig_range() minus migration, which nommu does not have
 * (CONFIG_CONTIG_ALLOC needs compaction or CMA, both MMU only).
 */
int __isolate_free_page(struct page *page, unsigned int order);
void __putback_isolated_page(struct page *page, unsigned int order, int mt);
void post_alloc_hook(struct page *page, unsigned int order, gfp_t gfp_flags);

#define RUN_MAXBLK	48

static struct zone *run_zone(void)
{
	struct zone *z;

	for_each_populated_zone(z)
		return z;		/* nommu, flat: ZONE_NORMAL only */
	return NULL;
}

/*
 * Take the n free pages at pfn s off the buddy free lists (zone lock held,
 * irqs off; the run must be made of free buddy blocks starting at s) and
 * make them ordinary order-0 pages, as alloc_pages_exact leaves them.
 * Returns false (nothing taken) if the run isn't free or a watermark says
 * no. Called with the lock held, returns with it released.
 *
 * Everything taken here is for processes (blocks, the pool growing, FDPIC
 * copies), so it leaves the kernel the pages up to the high watermark.
 * __isolate_free_page only keeps the min watermark, the same line the
 * kernel's own allocations stop at: the pool growing back for a swap-in
 * would take what the shrinker had just lent the kernel, free memory would
 * sit at min, and a slab page for /proc would run the OOM killer. Processes
 * get the pool, and swap. When swap can't make the room either (the pool
 * full of shared text and of processes that are running), the last try
 * goes down to the low watermark (reserve_low): the kernel keeps min..low,
 * and kswapd wakes. Without that last try, a growth a few KiB short fails
 * even with tens of KiB free above the high watermark.
 */
static struct task_struct *reserve_low;

static bool run_take(struct zone *zone, unsigned long s, unsigned long n, unsigned long flags)
{
	struct { struct page *page; unsigned int order; } blk[RUN_MAXBLK];
	unsigned long pfn, got = 0;
	int nb = 0, i;

	if (zone_page_state(zone, NR_FREE_PAGES) <
	    (reserve_low == current ? low_wmark_pages(zone) : high_wmark_pages(zone)) + n &&
	    system_state == SYSTEM_RUNNING) {
		st.reserve++;
		spin_unlock_irqrestore(&zone->lock, flags);
		return false;
	}
	for (pfn = s; got < n; ) {
		struct page *pg = pfn_to_page(pfn);
		unsigned int order;

		if (pfn >= zone_end_pfn(zone) || !PageBuddy(pg))
			goto undo;
		order = page_private(pg);	/* buddy_order() */
		if (nb == RUN_MAXBLK || !__isolate_free_page(pg, order))
			goto undo;		/* watermark */
		blk[nb].page = pg;
		blk[nb].order = order;
		nb++;
		pfn += 1UL << order;
		got += 1UL << order;
	}
	spin_unlock_irqrestore(&zone->lock, flags);
	for (i = 0; i < nb; i++) {
		post_alloc_hook(blk[i].page, blk[i].order, GFP_KERNEL);
		set_page_count(blk[i].page, 1);
		if (blk[i].order)
			split_page(blk[i].page, blk[i].order);
	}
	for (pfn = s + n; pfn < s + got; pfn++)	/* the last block's overhang */
		__free_page(pfn_to_page(pfn));
	return true;
undo:
	while (nb--)
		__putback_isolated_page(blk[nb].page, blk[nb].order,
					get_pageblock_migratetype(blk[nb].page));
	spin_unlock_irqrestore(&zone->lock, flags);
	return false;
}

/* best fit: the smallest run of free buddy blocks that holds n pages */
static unsigned long run_alloc(unsigned long n)
{
	unsigned long flags, pfn, end, s, best = 0, bestlen = ULONG_MAX;
	struct zone *zone = run_zone();

	if (!zone)
		return 0;
	drain_local_pages(zone);	/* free pages parked on the per-cpu lists */
	spin_lock_irqsave(&zone->lock, flags);
	pfn = zone->zone_start_pfn;
	end = zone_end_pfn(zone);
	while (pfn < end) {
		if (!PageBuddy(pfn_to_page(pfn))) {
			pfn++;
			continue;
		}
		for (s = pfn; pfn < end && PageBuddy(pfn_to_page(pfn)); )
			pfn += 1UL << page_private(pfn_to_page(pfn));
		if (pfn - s >= n && pfn - s < bestlen) {
			best = s;
			bestlen = pfn - s;
		}
	}
	if (bestlen == ULONG_MAX) {
		spin_unlock_irqrestore(&zone->lock, flags);
		return 0;
	}
	if (!run_take(zone, best, n, flags))
		return 0;
	return (unsigned long)page_address(pfn_to_page(best));
}

/* Extend the block at kva from size to nsize bytes in place, if the pages
 * just past it are free. (A free buddy block holding the first page past
 * the block starts there: the page before it is ours.) */
static bool run_extend(unsigned long kva, unsigned long size, unsigned long nsize)
{
	struct zone *zone = run_zone();
	unsigned long flags;

	if (atrans_pool_has(kva))
		return atrans_pool_extend(kva, nsize);
	if (!zone)
		return false;
	drain_local_pages(zone);
	spin_lock_irqsave(&zone->lock, flags);
	return run_take(zone, virt_to_pfn((void *)(kva + size)), (nsize - size) >> PAGE_SHIFT, flags);
}

/*
 * Take the free pages at kva, up to max bytes, as far as they are free
 * (the pool growing into the page allocator's memory just past its end:
 * the page before kva is the pool's, so a free buddy block holding kva
 * starts at kva). Returns the bytes taken.
 */
unsigned long atrans_run_take_at(unsigned long kva, unsigned long max)
{
	struct zone *zone = run_zone();
	unsigned long flags, pfn = virt_to_pfn((void *)kva), p, got = 0, n = max >> PAGE_SHIFT;
	int nb = 0;

	if (!zone || !n)
		return 0;
	drain_local_pages(zone);
	spin_lock_irqsave(&zone->lock, flags);
	for (p = pfn; got < n && nb < RUN_MAXBLK && p < zone_end_pfn(zone) &&
	     PageBuddy(pfn_to_page(p)); nb++) {
		got += 1UL << page_private(pfn_to_page(p));
		p = pfn + got;
	}
	n = min(n, got);
	if (!n) {
		spin_unlock_irqrestore(&zone->lock, flags);
		return 0;
	}
	if (!run_take(zone, pfn, n, flags))
		return 0;
	return n << PAGE_SHIFT;
}

static void drop_caches(void);

/*
 * For mm/nommu.c's do_mmap_private (FDPIC programs' copies and anonymous
 * memory): alloc_pages_exact wants a free power-of-two block, and without
 * compaction a few scattered pages can deny one while MemFree is high; then
 * the OOM killer runs. Try harder first: an exact run anywhere, then the
 * page cache dropped, then the ATRANS pool's free space lent, swapping
 * ATRANS processes out (the longest asleep first) to make some. Then
 * reclaim, but no OOM killer: the mmap (or the exec it is part of) fails
 * with ENOMEM, and the programs already running keep going.
 */
void *rp2350_alloc_exact(size_t size)
{
	unsigned long n = PAGE_ALIGN(size) >> PAGE_SHIFT;
	void *p = alloc_pages_exact(size, GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN);
	int i;

	if (!p)
		p = (void *)run_alloc(n);
	if (!p) {
		drop_caches();
		p = (void *)run_alloc(n);
	}
	if (!p && atrans_pool_lend(n))
		p = (void *)run_alloc(n);
	/* a block in the pool comes back as pool space to lend, one outside
	 * it as free pages */
	for (i = 0; !p && i < 8 && rp2350_atrans_swap_out_idle(); i++) {
		p = (void *)run_alloc(n);
		if (!p && atrans_pool_lend(n))
			p = (void *)run_alloc(n);
	}
	if (!p)
		p = alloc_pages_exact(size, GFP_KERNEL | __GFP_RETRY_MAYFAIL | __GFP_NOWARN);
	return p;
}

/* weak defaults, overridden by rp2350-atrans-swap.c */
bool __weak rp2350_atrans_swap_reclaim(unsigned long size)
{
	return false;
}

void __weak rp2350_atrans_swap_exit(struct mm_struct *mm)
{
}

bool __weak rp2350_atrans_swap_out_idle(void)
{
	return false;
}

int __weak rp2350_atrans_swap_diag(char *buf, int len)
{
	return scnprintf(buf, len, "no swap");
}

int __weak rp2350_atrans_swap_access(struct mm_struct *mm, unsigned long addr, void *buf,
				     int len, int write)
{
	return 0;
}

/*
 * Clean page cache sits in free memory as scattered pages, so the runs
 * a block needs are not there even with MemAvailable high (a compiler that
 * read libc.a and headers once). Drop it, as drop_caches=1 does, before
 * failing or swapping: fs/drop_caches.c's drop_pagecache_sb, which is static.
 */
static void drop_pagecache_sb(struct super_block *sb, void *unused)
{
	struct inode *inode, *toput = NULL;

	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		spin_lock(&inode->i_lock);
		if ((inode->i_state & (I_FREEING | I_WILL_FREE | I_NEW)) ||
		    (mapping_empty(inode->i_mapping) && !need_resched())) {
			spin_unlock(&inode->i_lock);
			continue;
		}
		__iget(inode);
		spin_unlock(&inode->i_lock);
		spin_unlock(&sb->s_inode_list_lock);
		invalidate_mapping_pages(inode->i_mapping, 0, -1);
		iput(toput);
		toput = inode;
		cond_resched();
		spin_lock(&sb->s_inode_list_lock);
	}
	spin_unlock(&sb->s_inode_list_lock);
	iput(toput);
}

static void drop_caches(void)
{
	lru_add_drain_all();
	iterate_supers(drop_pagecache_sb, NULL);
	drop_slab();
	st.drops++;
}

/* A new block with no stale cache lines for it (callers fill it next):
 * physically contiguous, exactly `size`. No OOM killer: the caller gets
 * ENOMEM. alloc_pages_exact is the fallback, as it may reclaim a little. */
#define BLOCK_WAIT	(3 * HZ)

unsigned long block_alloc(unsigned long size)
{
	unsigned long r0 = st.reserve, tries = 0;
	void *p = (void *)atrans_pool_alloc(size);	/* the pool first (rp2350-atrans-pool.c) */
	unsigned long flags, until;

	if (!p)
		p = (void *)run_alloc(size >> PAGE_SHIFT);
	if (!p)
		p = alloc_pages_exact(size, GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN);
	if (!p)
		p = (void *)run_alloc(size >> PAGE_SHIFT);
	if (!p) {
		drop_caches();		/* the pool may grow into what this frees */
		p = (void *)atrans_pool_alloc(size);
		if (!p)
			p = (void *)run_alloc(size >> PAGE_SHIFT);
	}
	/* swap other processes out (may sleep; callers re-check that their
	 * block is loaded after block_alloc). For BLOCK_WAIT at most: each
	 * swap-out can succeed and the room still go elsewhere (to the
	 * process just swapped out, waking), and then the exec, fork or
	 * growth fails with ENOMEM instead of waiting for ever. */
	until = jiffies + BLOCK_WAIT;
	while (!p && (++tries, rp2350_atrans_swap_reclaim(size))) {
		p = (void *)atrans_pool_alloc(size);	/* swap-out may have freed pool space */
		if (!p)
			p = (void *)run_alloc(size >> PAGE_SHIFT);
		if (!p && (time_after(jiffies, until) || fatal_signal_pending(current))) {
			st.gaveup++;
			break;
		}
	}
	if (!p) {
		/* swap can't make the room: down to the low watermark */
		struct task_struct *prev = reserve_low;

		reserve_low = current;
		p = (void *)atrans_pool_alloc(size);
		if (!p)
			p = (void *)run_alloc(size >> PAGE_SHIFT);
		reserve_low = prev;
		if (p)
			st.lowres++;
	}
	if (!p) {
		struct zone *z = run_zone();
		unsigned long ps, pf, pl;
		char d[200];

		st.nomem++;
		atrans_pool_diag(&ps, &pf, &pl);
		rp2350_atrans_swap_diag(d, sizeof(d));
		pr_info_ratelimited("rp2350-atrans: %s[%d]: no %lu KiB block: pool %lu KiB, free %lu, largest %lu; free pages %lu KiB (min %lu low %lu high %lu); %lu runs refused for the kernel's reserve; %lu swap rounds: %s\n",
				    current->comm, task_pid_nr(current), size >> 10, ps >> 10, pf >> 10,
				    pl >> 10, z ? zone_page_state(z, NR_FREE_PAGES) << 2 : 0,
				    z ? min_wmark_pages(z) << 2 : 0, z ? low_wmark_pages(z) << 2 : 0,
				    z ? high_wmark_pages(z) << 2 : 0, st.reserve - r0, tries, d);
		return 0;
	}
	local_irq_save(flags);
	xip_flush_all();
	local_irq_restore(flags);
	st.live++;
	st.live_bytes += size;
	return (unsigned long)p;
}

/* the page allocator's run allocator, for the pool (taken at boot) */
unsigned long __init block_alloc_run(unsigned long size)
{
	return run_alloc(size >> PAGE_SHIFT);
}

void block_free(unsigned long kva, unsigned long size)
{
	if (atrans_pool_has(kva))
		atrans_pool_free(kva);
	else
		free_pages_exact((void *)kva, size);
	st.live--;
	st.live_bytes -= size;
}

void set_block(struct mm_struct *mm, unsigned long kva, unsigned long size)
{
	/* (nommu keeps no RSS counters, so the OOM killer can't weigh this) */
	mm->context.atrans_kva = kva;
	mm->context.atrans_size = size;
	mm->total_vm = size >> PAGE_SHIFT;
	if (kva && atrans_pool_has(kva))
		atrans_pool_own(kva, mm, NULL);		/* movable from now on */
}

/*
 * The task allowed to have its own process's block and text moved while it
 * allocates (atrans_grow, fork): it holds its mmap lock and touches its
 * window only through ensure_loaded() after block_alloc(). Saved and
 * restored around the allocation; another task sees it only as "not me".
 */
static struct task_struct *self_move;

/* May the pool move this block now? (rp2350-atrans-pool.c) */
bool atrans_may_move(struct mm_struct *mm, struct atrans_text *t)
{
	struct mm_struct *cur = current->mm;

	if (mm)
		return mm != cur || self_move == current;
	if (t)
		return t->users && (!cur || !cur->context.atrans || cur->context.atrans->text != t ||
				    self_move == current);
	return false;			/* being filled */
}

/*
 * Move a block that isn't in use from src to dst (the pool's compaction,
 * rp2350-atrans-pool.c): mm's data block, or the shared text t. Another
 * process's block only if its mmap lock is free (not growing, forking,
 * being swapped out or read through /proc); the current process's only
 * when it allows it (self_move), and then the windows are repointed at
 * once. Otherwise at the next switch to the process. Copied through the
 * uncached alias after a full cache flush. Nothing here sleeps, so no task
 * runs meanwhile (UP, not preemptible).
 */
bool atrans_block_move(struct mm_struct *mm, struct atrans_text *t, unsigned long src,
		       unsigned long dst, unsigned long size)
{
	struct mm_struct *cur = current->mm;
	bool locked = false, self;
	unsigned long flags;

	if (!atrans_may_move(mm, t))
		return false;
	if (mm) {
		self = mm == cur;
		if (!self) {
			if (!down_write_trylock(&mm->mmap_lock))
				return false;
			locked = true;
		}
		if (mm->context.atrans_kva != src || mm->context.atrans_size != size ||
		    (mm->context.atrans && mm->context.atrans->swap))
			goto no;
	} else {
		if (t->kva != src || t->size != size)
			return false;
		self = cur && cur->context.atrans && cur->context.atrans->text == t;
	}
	local_irq_save(flags);
	xip_flush_all();		/* its lines, window or kva, to PSRAM */
	if (mm && loaded == src)
		atrans_map(0, 0);
	if (t && loaded_text == src) {
		/* ATRANS7 is rewritten only along with ATRANS4: unload both */
		atrans_map(0, 0);
		writel_relaxed(0, QMI_ATRANS7);
		readl_relaxed(QMI_ATRANS7);
		loaded_text = 0;
	}
	local_irq_restore(flags);
	memmove(notrans(dst), notrans(src), size);
	mb();
	local_irq_save(flags);
	if (mm)
		mm->context.atrans_kva = dst;
	else
		t->kva = dst;
	if (self) {
		/* current goes on running: its windows, now */
		atrans_map(cur->context.atrans_kva, cur->context.atrans_size);
		text_map(cur);
		mpu_confine(cur);
		tp_store(current_thread_info()->tp_value[0]);
	}
	local_irq_restore(flags);
	if (locked)
		up_write(&mm->mmap_lock);
	return true;
no:
	if (locked)
		up_write(&mm->mmap_lock);
	return false;
}

/* end of what is in use: brk or the highest mapped page */
unsigned long used_top(struct mm_struct *mm)
{
	struct rp2350_atrans_mm *a = mm->context.atrans;
	unsigned long top = PAGE_ALIGN(mm->brk), last;

	last = find_last_bit(a->map, ATRANS_PAGES);
	if (last < ATRANS_PAGES)
		top = max(top, ATRANS_WINDOW + ((last + 1) << PAGE_SHIFT));
	return top;
}

static void set_end_brk(struct mm_struct *mm)
{
	struct rp2350_atrans_mm *a = mm->context.atrans;
	unsigned long first = find_first_bit(a->map, ATRANS_PAGES);

	mm->context.end_brk = ATRANS_WINDOW + (first << PAGE_SHIFT);
}

/*
 * Copy what mm uses from the window (mm is current and loaded) to the block
 * at dst, through its uncached alias: the stack from sp up (all of it if sp
 * is elsewhere or other tasks share mm), image and heap to brk, and the
 * mapped pages. Returns the number of bytes copied.
 */
/*
 * The parts of mm's data block in use, as window offsets: the stack from sp
 * up (all of it if sp is elsewhere or other tasks share mm), image and heap
 * to brk, and the mapped runs. Shared text is not part of it. Used by fork,
 * growth and swap-out.
 */
void atrans_for_each_used(struct mm_struct *mm, unsigned long sp,
			  void (*fn)(unsigned long off, unsigned long len, void *arg), void *arg)
{
	struct rp2350_atrans_mm *a = mm->context.atrans;
	unsigned long lo = ATRANS_WINDOW, hi = ALIGN(mm->brk, 4);
	unsigned int rs, re;

	if (atomic_read(&mm->mm_users) == 1 && sp >= ATRANS_WINDOW && sp < a->stack_top)
		lo = sp & ~7UL;
	fn(lo - ATRANS_WINDOW, hi - lo, arg);
	for_each_set_bitrange(rs, re, a->map, ATRANS_PAGES)
		fn((unsigned long)rs << PAGE_SHIFT, (unsigned long)(re - rs) << PAGE_SHIFT, arg);
}

struct copy_arg {
	unsigned long dst, n;
};

static void copy_range(unsigned long off, unsigned long len, void *v)
{
	struct copy_arg *c = v;

	memcpy(notrans(c->dst) + off, (void *)(ATRANS_WINDOW + off), len);
	c->n += len;
}

static unsigned long copy_used(struct mm_struct *mm, unsigned long dst, unsigned long sp)
{
	struct copy_arg c = { dst, 0 };

	atrans_for_each_used(mm, sp, copy_range, &c);
	mb();
	return c.n;
}

/* Make sure current's block is loaded (it is, unless something is badly
 * off), before reading it through the window. */
void ensure_loaded(struct mm_struct *mm)
{
	unsigned long flags;

	local_irq_save(flags);
	if (loaded != mm->context.atrans_kva) {
		xip_flush_all();
		atrans_map(mm->context.atrans_kva, mm->context.atrans_size);
		text_map(mm);
		mpu_confine(mm);
		tp_store(current_thread_info()->tp_value[0]);
	}
	local_irq_restore(flags);
}

/* Size to grow to when `need` bytes of window are needed: half as big
 * again, plus slack, but never a bigger power-of-two run than `need`
 * itself needs (that is what alloc_pages_exact has to find free). */
static unsigned long grow_target(unsigned long old, unsigned long need)
{
	unsigned long want = max(need + ATRANS_SLACK, old + old / 2);

	want = PAGE_ALIGN(min(want, ATRANS_MAX));
	if (get_order(want) > get_order(need))
		want = min(PAGE_SIZE << get_order(need), ATRANS_MAX);
	return max(want, need);
}

/*
 * Grow current's block to cover at least `need` bytes of window: allocate,
 * copy what is in use, repoint ATRANS4 and the MPU, free the old block.
 * mm == current->mm, mmap lock held for writing.
 */
static int atrans_grow(struct mm_struct *mm, unsigned long need)
{
	unsigned long old = mm->context.atrans_size, okva = mm->context.atrans_kva;
	unsigned long size, kva, flags, n;
	struct task_struct *prev;
	u32 c0, c;
	int i;

	need = PAGE_ALIGN(need);
	if (need <= old)
		return 0;
	if (need > ATRANS_MAX || mm != current->mm)
		return -ENOMEM;
	size = grow_target(old, need);
	/* in place, when the pages after the block are free (in the pool: can
	 * be made free, moving this block too): no copy, and no second block
	 * while the first is still held */
	prev = self_move;
	self_move = current;
	n = run_extend(okva, old, size);
	if (!n && size > need) {
		okva = mm->context.atrans_kva;
		n = run_extend(okva, old, size = need);
	}
	/*
	 * Still in place, after swapping others out to make room for just the
	 * growth: a second block means old and new at once (a 640 KiB block
	 * growing by 16 KiB would ask swap for 656 KiB, which a full pool may
	 * have no way to give).
	 */
	for (i = 0; !n && i < 3 && rp2350_atrans_swap_reclaim(need - old); i++)
		n = run_extend(mm->context.atrans_kva, old, size = need);
	self_move = prev;
	okva = mm->context.atrans_kva;		/* the pool may have moved it */
	if (n) {
in_place:
		local_irq_save(flags);
		xip_flush_all();	/* stale lines under the new pages' kva */
		atrans_map(okva, size);
		set_block(mm, okva, size);
		mpu_confine(mm);
		local_irq_restore(flags);
		st.live_bytes += size - old;
		st.grows_inplace++;
		return 0;
	}
	size = grow_target(old, need);
	prev = self_move;
	self_move = current;		/* the pool may move this block meanwhile */
	kva = block_alloc(size);
	if (!kva && size > need)
		kva = block_alloc(size = need);
	self_move = prev;
	if (!kva) {
		/* no second block: in place, at the low watermark */
		struct task_struct *rprev = reserve_low;

		prev = self_move;
		self_move = current;
		reserve_low = current;
		n = run_extend(mm->context.atrans_kva, old, size = need);
		reserve_low = rprev;
		self_move = prev;
		okva = mm->context.atrans_kva;
		if (n) {
			st.lowres++;
			goto in_place;
		}
		pr_info_ratelimited("rp2350-atrans: %s[%d] can't grow from %lu to %lu KiB\n",
				    current->comm, task_pid_nr(current), old >> 10, need >> 10);
		return -ENOMEM;
	}
	okva = mm->context.atrans_kva;
	ensure_loaded(mm);
	c0 = cycles();
	/* nothing below sleeps: no other task can touch the window meanwhile */
	n = copy_used(mm, kva, current_pt_regs()->ARM_sp);
	local_irq_save(flags);
	xip_flush_all();		/* window lines belong to the old block */
	atrans_map(kva, size);
	set_block(mm, kva, size);
	mpu_confine(mm);
	tp_store(current_thread_info()->tp_value[0]);	/* not copied */
	local_irq_restore(flags);
	block_free(okva, old);
	c = cycles() - c0;
	st.grows++;
	st.grow_bytes += n;
	st.grow_cycles += c;
	if (c > st.grow_max)
		st.grow_max = c;
	return 0;
}

/* Give back the top of current's block in place when much of it is unused
 * (after munmap or a brk shrink). mmap lock held for writing. */
static void atrans_maybe_shrink(struct mm_struct *mm)
{
	unsigned long size = mm->context.atrans_size, kva = mm->context.atrans_kva;
	unsigned long used, nsize, flags;

	if (mm != current->mm)
		return;
	used = used_top(mm) - ATRANS_WINDOW;
	if (size - used < max(ATRANS_SHRINK, size / 4))
		return;
	nsize = PAGE_ALIGN(used + ATRANS_SLACK);
	if (nsize >= size)
		return;
	ensure_loaded(mm);
	local_irq_save(flags);
	xip_flush_all();		/* no dirty lines may land in the freed tail */
	atrans_map(kva, nsize);
	set_block(mm, kva, nsize);
	mpu_confine(mm);
	local_irq_restore(flags);
	if (atrans_pool_has(kva))
		atrans_pool_trim(kva, nsize);
	else
		free_pages_exact((void *)(kva + nsize), size - nsize);
	st.live_bytes -= size - nsize;
	st.shrinks++;
}

/* ---- shared text ------------------------------------------------------------- */

/*
 * The text of the program being exec'd (its PT_LOADs in the text window,
 * all read-only, ending at thi): an existing block for the same file, or a
 * new one filled from it. Filled through the cached kernel alias and then
 * written back: no window maps the new block yet.
 */
static struct atrans_text *text_get(struct linux_binprm *bprm, struct elf32_phdr *ph, int n,
				    unsigned long tlo, unsigned long thi)
{
	struct inode *inode = file_inode(bprm->file);
	struct timespec64 mtime = inode_get_mtime(inode);
	unsigned long size = PAGE_ALIGN(thi - ATRANS_TEXT), flags;
	struct atrans_text *t;
	loff_t pos;
	ssize_t r;
	int i;

	mutex_lock(&text_lock);
	list_for_each_entry(t, &texts, list) {
		if (t->inode == inode && t->isize == i_size_read(inode) &&
		    timespec64_equal(&t->mtime, &mtime) && t->size == size) {
			t->users++;
			st.text_hits++;
			goto out;
		}
	}
	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t) {
		t = ERR_PTR(-ENOMEM);
		goto out;
	}
	t->kva = block_alloc(size);
	if (!t->kva) {
		kfree(t);
		t = ERR_PTR(-ENOMEM);
		goto out;
	}
	memset((void *)t->kva, 0, size);
	for (i = 0; i < n; i++) {
		if (ph[i].p_type != PT_LOAD || ph[i].p_vaddr < ATRANS_TEXT || !ph[i].p_filesz)
			continue;
		pos = ph[i].p_offset;
		r = kernel_read(bprm->file, (void *)(t->kva + ph[i].p_vaddr - ATRANS_TEXT),
				ph[i].p_filesz, &pos);
		if (r != ph[i].p_filesz) {
			block_free(t->kva, size);
			kfree(t);
			t = ERR_PTR(r < 0 ? r : -EIO);
			goto out;
		}
	}
	local_irq_save(flags);
	xip_flush_all();		/* to PSRAM; no lines left under the kva */
	local_irq_restore(flags);
	t->size = size;
	t->start = tlo;
	t->end = thi;
	t->inode = inode;
	ihold(inode);
	t->isize = i_size_read(inode);
	t->mtime = mtime;
	t->users = 1;
	list_add(&t->list, &texts);
	if (atrans_pool_has(t->kva))
		atrans_pool_own(t->kva, NULL, t);	/* filled: movable from now on */
	st.texts++;
	st.text_bytes += size;
	st.text_loads++;
out:
	mutex_unlock(&text_lock);
	return t;
}

static void text_hold(struct atrans_text *t)
{
	mutex_lock(&text_lock);
	t->users++;
	mutex_unlock(&text_lock);
}

static void text_put(struct atrans_text *t)
{
	unsigned long flags;

	mutex_lock(&text_lock);
	if (--t->users) {
		mutex_unlock(&text_lock);
		return;
	}
	list_del(&t->list);
	mutex_unlock(&text_lock);
	local_irq_save(flags);
	if (loaded_text == t->kva) {
		/* its lines are clean, and ATRANS7 is next set after a flush */
		writel_relaxed(0, QMI_ATRANS7);
		readl_relaxed(QMI_ATRANS7);
		loaded_text = 0;
	}
	local_irq_restore(flags);
	block_free(t->kva, t->size);
	st.texts--;
	st.text_bytes -= t->size;
	iput(t->inode);
	kfree(t);
}

/* ---- brk, mmap, munmap (hooks in mm/nommu.c) ------------------------------------ */

unsigned long rp2350_atrans_brk(struct mm_struct *mm, unsigned long brk)
{
	unsigned long ret;

	if (mmap_write_lock_killable(mm))
		return mm->brk;
	ret = mm->brk;
	if (brk < mm->start_brk || brk > mm->context.end_brk)
		goto out;
	if (brk > ATRANS_WINDOW + mm->context.atrans_size &&
	    atrans_grow(mm, brk - ATRANS_WINDOW))
		goto out;
	if (brk > mm->brk)
		memset((void *)mm->brk, 0, brk - mm->brk);	/* may reuse unmapped pages */
	ret = mm->brk;
	mm->brk = brk;
	if (brk < ret)
		atrans_maybe_shrink(mm);
	ret = brk;
out:
	mmap_write_unlock(mm);
	return ret;
}

/*
 * Where mmap puts pages: 0, the highest free run of the block (room for brk
 * to grow); 1, the lowest free run from ATRANS_BRKROOM above brk, so that
 * the mapped pages stay packed low, the block's top stays free to shrink,
 * and a fork child (which needs the block up to the highest mapped page)
 * stays small. /sys/module/rp2350_atrans/parameters/mmap_low.
 */
static int mmap_low = 1;
module_param(mmap_low, int, 0644);
#define ATRANS_BRKROOM	(16 * 1024UL)

/* lowest run of n clear bits in [lo, hi), or -1 */
static long find_run_low(unsigned long *map, unsigned long lo, unsigned long hi, unsigned long n)
{
	unsigned long i, z;

	for (i = lo; i + n <= hi; i = z + 1) {
		i = find_next_zero_bit(map, hi, i);
		if (i + n > hi)
			return -1;
		z = find_next_bit(map, i + n, i);
		if (z >= i + n)
			return i;
	}
	return -1;
}

/* highest run of n clear bits in [lo, hi), or -1 */
static long find_run(unsigned long *map, unsigned long lo, unsigned long hi, unsigned long n)
{
	unsigned long first, i;

	if (hi < lo + n)
		return -1;
	first = hi - n;
	for (;;) {
		i = find_next_bit(map, first + n, first);
		if (i >= first + n)
			return first;
		if (i < lo + n)
			return -1;
		first = i - n;
	}
}

/*
 * mmap for ATRANS processes (do_mmap hook, mmap lock held for writing;
 * current is the process, so the window shows its block). Pages come from
 * [page above brk, block top), highest free run first; if there is none
 * the block grows. They are zeroed or filled from the file: every mapping
 * is a private copy. prot is not enforced (one MPU region covers the
 * block). Writable MAP_SHARED is refused: it could not stay shared across
 * fork.
 */
unsigned long rp2350_atrans_mmap(struct file *file, unsigned long addr, unsigned long len,
				 unsigned long prot, unsigned long flags, unsigned long pgoff)
{
	struct mm_struct *mm = current->mm;
	struct rp2350_atrans_mm *a = mm->context.atrans;
	unsigned long lo = (PAGE_ALIGN(mm->brk) - ATRANS_WINDOW) >> PAGE_SHIFT, n, t;
	long first;
	loff_t pos;
	ssize_t got;
	unsigned long dev;

	/* a device that maps directly (/dev/pioN): rp2350-atrans-dev.c */
	dev = rp2350_atrans_dev_mmap(file, len, prot, flags, pgoff);
	if (dev != (unsigned long)-ENOSYS)
		return dev;
	if (!len || (flags & MAP_TYPE) == MAP_SHARED_VALIDATE)
		return -EINVAL;
	if ((flags & MAP_TYPE) == MAP_SHARED && (prot & PROT_WRITE))
		return -EINVAL;
	if (len > ATRANS_MAX)
		return -ENOMEM;
	len = PAGE_ALIGN(len);
	n = len >> PAGE_SHIFT;

	if (flags & MAP_FIXED) {
		if (addr & ~PAGE_MASK)
			return -EINVAL;
		if (addr < ATRANS_WINDOW + (lo << PAGE_SHIFT) || addr - ATRANS_WINDOW > ATRANS_MAX - len)
			return -ENOMEM;
		first = (addr - ATRANS_WINDOW) >> PAGE_SHIFT;
		if (addr + len > ATRANS_WINDOW + mm->context.atrans_size &&
		    atrans_grow(mm, addr + len - ATRANS_WINDOW))
			return -ENOMEM;
	} else if (mmap_low) {
		unsigned long lo2 = lo + (ATRANS_BRKROOM >> PAGE_SHIFT);

		first = find_run_low(a->map, lo2, mm->context.atrans_size >> PAGE_SHIFT, n);
		if (first < 0) {
			/* grow: the free pages at the old top plus new ones */
			t = max(lo2, mm->context.atrans_size >> PAGE_SHIFT);
			while (t > lo2 && !test_bit(t - 1, a->map))
				t--;
			if (t + n > ATRANS_PAGES || atrans_grow(mm, (t + n) << PAGE_SHIFT))
				return -ENOMEM;
			first = find_run_low(a->map, lo2, mm->context.atrans_size >> PAGE_SHIFT, n);
			if (first < 0)
				return -ENOMEM;
		}
	} else {
		first = find_run(a->map, lo, mm->context.atrans_size >> PAGE_SHIFT, n);
		if (first < 0) {
			/* grow: the free pages at the old top plus new ones */
			t = max(lo, mm->context.atrans_size >> PAGE_SHIFT);
			while (t > lo && !test_bit(t - 1, a->map))
				t--;
			if (t + n > ATRANS_PAGES || atrans_grow(mm, (t + n) << PAGE_SHIFT))
				return -ENOMEM;
			first = find_run(a->map, lo, mm->context.atrans_size >> PAGE_SHIFT, n);
			if (first < 0)
				return -ENOMEM;
		}
	}
	addr = ATRANS_WINDOW + ((unsigned long)first << PAGE_SHIFT);
	memset((void *)addr, 0, len);
	if (file) {
		pos = (loff_t)pgoff << PAGE_SHIFT;
		got = kernel_read(file, (void *)addr, len, &pos);
		if (got < 0)
			return got;
	}
	bitmap_set(a->map, first, n);
	set_end_brk(mm);
	st.mmaps++;
	return addr;
}

int rp2350_atrans_munmap(struct mm_struct *mm, unsigned long start, size_t len)
{
	struct rp2350_atrans_mm *a = mm->context.atrans;
	unsigned long first, last, end;

	if ((start & ~PAGE_MASK) || !len)
		return -EINVAL;
	if (rp2350_atrans_dev_munmap(mm, start, len))
		return 0;
	end = PAGE_ALIGN(start + len);
	if (end <= start)
		return -EINVAL;
	if (start >= ATRANS_WEND || end <= ATRANS_WINDOW)
		return 0;			/* nothing of ours there: not an error */
	first = start < ATRANS_WINDOW ? 0 : (start - ATRANS_WINDOW) >> PAGE_SHIFT;
	last = min(ATRANS_PAGES, (end - ATRANS_WINDOW) >> PAGE_SHIFT);
	bitmap_clear(a->map, first, last - first);
	set_end_brk(mm);
	st.munmaps++;
	atrans_maybe_shrink(mm);
	return 0;
}

/* ---- fork, exit (hooks in kernel/fork.c and mm/nommu.c) ---------------------- */

/*
 * dup_mmap (nommu) for clone() without CLONE_VM. ATRANS processes get a
 * copy of their block. Stock nommu would give a plain nommu process an
 * empty mm while it keeps running in the parent's memory: refuse that.
 */
int rp2350_atrans_dup(struct mm_struct *oldmm, struct mm_struct *mm)
{
	struct rp2350_atrans_mm *a;
	unsigned long kva, used, size, n;
	struct task_struct *prev;
	u32 c0, c;

	/* dup_mm copied the parent's mm, context and counters included */
	mm->context.atrans_kva = 0;
	mm->context.atrans_size = 0;
	mm->context.atrans = NULL;
	mm->total_vm = 0;
	if (!oldmm->context.atrans_kva || oldmm != current->mm)
		return -EINVAL;
	a = kmemdup(oldmm->context.atrans, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	if (a->text)
		text_hold(a->text);		/* shared, not copied */
	rp2350_atrans_dev_dup(a);		/* the same device ranges */
	mm->context.atrans = a;			/* exit_mmap frees it on failure */
	used = used_top(oldmm) - ATRANS_WINDOW;
	size = min(oldmm->context.atrans_size, PAGE_ALIGN(used + ATRANS_SLACK));
	prev = self_move;
	self_move = current;		/* the pool may move the parent meanwhile */
	kva = block_alloc(size);
	if (!kva && size > used)
		kva = block_alloc(size = used);
	self_move = prev;
	if (!kva) {
		pr_info_ratelimited("rp2350-atrans: %s[%d] fork: no %lu KiB block\n",
				    current->comm, task_pid_nr(current), size >> 10);
		return -ENOMEM;
	}
	ensure_loaded(oldmm);
	c0 = cycles();
	n = copy_used(oldmm, kva, current_pt_regs()->ARM_sp);
	set_block(mm, kva, size);
	c = cycles() - c0;
	st.forks++;
	st.fork_bytes += n;
	st.fork_cycles += c;
	if (c > st.fork_max)
		st.fork_max = c;
	return 0;
}

void rp2350_atrans_exit(struct mm_struct *mm)
{
	struct rp2350_atrans_mm *a = mm->context.atrans;
	unsigned long kva = mm->context.atrans_kva, flags;

	if (kva) {
		local_irq_save(flags);
		if (loaded == kva) {
			/* write back before the block is freed: a dirty line
			 * evicted later would land in whoever gets it next */
			xip_flush_all();
			atrans_map(0, 0);
		}
		local_irq_restore(flags);
		block_free(kva, mm->context.atrans_size);
		set_block(mm, 0, 0);
	}
	if (a) {
		/* the swap slots (a->swap), while mm->context.atrans is still set */
		rp2350_atrans_swap_exit(mm);
		mm->context.atrans = NULL;
		if (a->text)
			text_put(a->text);
		rp2350_atrans_dev_exit(a);
		st.exits++;
	}
	kfree(a);
}

/* ---- other processes' memory: /proc/<pid>/cmdline, environ, mem, ptrace ---- */

/* __access_remote_vm hook (mm/nommu.c): through the window if the block is
 * loaded (its lines may be dirty), else through the uncached alias. */
int rp2350_atrans_access(struct mm_struct *mm, unsigned long addr, void *buf, int len, int write)
{
	unsigned long kva, size;
	void *p;

	if (mmap_read_lock_killable(mm))
		return 0;
	kva = mm->context.atrans_kva;
	size = mm->context.atrans_size;
	if (!kva && mm->context.atrans && !(mm_tsize(mm) && addr >= ATRANS_TEXT)) {
		len = rp2350_atrans_swap_access(mm, addr, buf, len, write);	/* swapped out */
	} else if (len > 0 && !write && mm_tsize(mm) && addr >= ATRANS_TEXT &&
	    addr < ATRANS_TEXT + mm_tsize(mm)) {
		/* text is never dirty in the cache: read its block uncached */
		struct atrans_text *t = mm->context.atrans->text;

		len = min_t(unsigned long, len, ATRANS_TEXT + t->size - addr);
		memcpy(buf, notrans(t->kva) + (addr - ATRANS_TEXT), len);
	} else if (!kva || len <= 0 || addr < ATRANS_WINDOW + (write ? ATRANS_TPSIZE : 0) ||
	    addr >= ATRANS_WINDOW + size) {
		len = 0;
	} else {
		len = min_t(unsigned long, len, ATRANS_WINDOW + size - addr);
		/* nothing below sleeps, so the window keeps showing what it shows */
		p = kva == loaded ? (void *)addr : notrans(kva) + (addr - ATRANS_WINDOW);
		if (write)
			memcpy(p, buf, len);
		else
			memcpy(buf, p, len);
	}
	mmap_read_unlock(mm);
	return len;
}

/* /proc/<pid>/maps (fs/proc/task_nommu.c hook), mmap lock held */
static void map_line(struct seq_file *m, unsigned long s, unsigned long e, const char *perm,
		     struct file *f, const char *name)
{
	if (s >= e)
		return;
	seq_printf(m, "%08lx-%08lx %s 00000000 00:00 0", s, e, perm);
	if (f) {
		seq_pad(m, ' ');
		seq_file_path(m, f, "\n");
	} else if (name) {
		seq_pad(m, ' ');
		seq_puts(m, name);
	}
	seq_putc(m, '\n');
}

int rp2350_atrans_show_maps(struct seq_file *m, struct mm_struct *mm)
{
	struct rp2350_atrans_mm *a = mm->context.atrans;
	unsigned long top = ATRANS_WINDOW + mm->context.atrans_size;
	struct file *exe;
	unsigned int rs, re;

	if (!a)
		return 0;
	exe = get_mm_exe_file(mm);
	map_line(m, ATRANS_WINDOW, a->stack_top, "rw-p", NULL, "[stack]");
	map_line(m, a->stack_top, mm->start_brk, a->text ? "rw-p" : "rwxp", exe, NULL);
	map_line(m, mm->start_brk, PAGE_ALIGN(mm->brk), "rw-p", NULL, "[heap]");
	for_each_set_bitrange(rs, re, a->map, ATRANS_PAGES)
		map_line(m, ATRANS_WINDOW + ((unsigned long)rs << PAGE_SHIFT),
			 ATRANS_WINDOW + ((unsigned long)re << PAGE_SHIFT), "rw-p", NULL, NULL);
	seq_printf(m, "%08lx-%08lx ---p 00000000 00:00 0", top, ATRANS_WEND);
	seq_pad(m, ' ');
	if (mm->context.atrans_kva)
		seq_printf(m, "[block %#lx, %lu KiB]\n", mm->context.atrans_kva - CONFIG_DRAM_BASE,
			   mm->context.atrans_size >> 10);
	else
		seq_puts(m, "[swapped out]\n");
	if (a->text) {
		map_line(m, a->text->start & PAGE_MASK, a->text->end, "r-xs", exe, NULL);
		seq_printf(m, "%08lx-%08lx ---p 00000000 00:00 0", ATRANS_TEXT + a->text->size,
			   ATRANS_TEND);
		seq_pad(m, ' ');
		seq_printf(m, "[text %#lx, %lu KiB, %d users]\n", a->text->kva - CONFIG_DRAM_BASE,
			   a->text->size >> 10, a->text->users);
	}
	rp2350_atrans_dev_show(m, mm);
	if (exe)
		fput(exe);
	return 0;
}

unsigned long rp2350_atrans_vsize(struct mm_struct *mm)
{
	return mm->context.atrans_size + mm_tsize(mm);
}

/* ---- loader: static ELF linked in the window --------------------------------- */

static struct linux_binfmt atrans_format;

#define AUXV_N	20

/*
 * Below the strings (argv, envp, the file name; transfer_args_to_stack put
 * them under stack_top): AT_RANDOM bytes, a copy of the program headers if
 * no PT_LOAD holds them, then argc, argv, envp and auxv, sp 8-aligned.
 */
static int atrans_tables(struct linux_binprm *bprm, struct mm_struct *mm,
			 struct elf32_hdr *eh, struct elf32_phdr *ph, unsigned long phdr_va)
{
	unsigned long auxv[AUXV_N * 2], *av = auxv, rnd, sp = mm->start_stack;
	char __user *p = (char __user *)mm->start_stack;
	unsigned long __user *u;
	u8 rbytes[16];
	long i, len, n;

	sp &= ~15UL;
	sp -= sizeof(rbytes);
	rnd = sp;
	get_random_bytes(rbytes, sizeof(rbytes));
	if (copy_to_user((void __user *)rnd, rbytes, sizeof(rbytes)))
		return -EFAULT;
	if (!phdr_va) {
		sp -= eh->e_phnum * sizeof(*ph);
		sp &= ~7UL;
		phdr_va = sp;
		if (copy_to_user((void __user *)phdr_va, ph, eh->e_phnum * sizeof(*ph)))
			return -EFAULT;
	}

#define AUX(t, v) do { *av++ = (t); *av++ = (v); } while (0)
	AUX(AT_PAGESZ, PAGE_SIZE);
	AUX(AT_PHDR, phdr_va);
	AUX(AT_PHENT, sizeof(*ph));
	AUX(AT_PHNUM, eh->e_phnum);
	AUX(AT_BASE, 0);
	AUX(AT_FLAGS, 0);
	AUX(AT_ENTRY, eh->e_entry);
	AUX(AT_HWCAP, 0);
	AUX(AT_CLKTCK, CLOCKS_PER_SEC);
	AUX(AT_UID, from_kuid_munged(current_user_ns(), current_uid()));
	AUX(AT_EUID, from_kuid_munged(current_user_ns(), current_euid()));
	AUX(AT_GID, from_kgid_munged(current_user_ns(), current_gid()));
	AUX(AT_EGID, from_kgid_munged(current_user_ns(), current_egid()));
	AUX(AT_SECURE, bprm->secureexec);
	AUX(AT_RANDOM, rnd);
	AUX(AT_EXECFN, bprm->exec);
	AUX(AT_NULL, 0);
#undef AUX
	n = av - auxv;

	sp -= (1 + bprm->argc + 1 + bprm->envc + 1 + n) * sizeof(long);
	sp &= ~7UL;
	if (sp < ATRANS_WINDOW + ATRANS_TPSIZE + 1024)
		return -E2BIG;
	mm->start_stack = sp;
	u = (unsigned long __user *)sp;

	if (put_user(bprm->argc, u++))
		return -EFAULT;
	mm->arg_start = (unsigned long)p;
	for (i = bprm->argc; i > 0; i--) {
		if (put_user((unsigned long)p, u++))
			return -EFAULT;
		len = strnlen_user(p, MAX_ARG_STRLEN);
		if (!len || len > MAX_ARG_STRLEN)
			return -EINVAL;
		p += len;
	}
	if (put_user(0, u++))
		return -EFAULT;
	mm->arg_end = mm->env_start = (unsigned long)p;
	for (i = bprm->envc; i > 0; i--) {
		if (put_user((unsigned long)p, u++))
			return -EFAULT;
		len = strnlen_user(p, MAX_ARG_STRLEN);
		if (!len || len > MAX_ARG_STRLEN)
			return -EINVAL;
		p += len;
	}
	if (put_user(0, u++))
		return -EFAULT;
	mm->env_end = (unsigned long)p;
	if (copy_to_user(u, auxv, n * sizeof(long)))
		return -EFAULT;
	return 0;
}

static int load_atrans_binary(struct linux_binprm *bprm)
{
	struct elf32_hdr *eh = (struct elf32_hdr *)bprm->buf;
	struct elf32_phdr *ph = NULL;
	unsigned long lo = ATRANS_WEND, hi = 0, tlo = ATRANS_TEND, thi = 0, phdr_va = 0, size, kva;
	struct rp2350_atrans_mm *a;
	struct atrans_text *t;
	struct mm_struct *mm;
	loff_t pos;
	int i, ret;

	if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS32 ||
	    eh->e_ident[EI_DATA] != ELFDATA2LSB || eh->e_type != ET_EXEC ||
	    eh->e_machine != EM_ARM || eh->e_phentsize != sizeof(*ph) ||
	    !eh->e_phnum || eh->e_phnum > 16)
		return -ENOEXEC;
	ph = kmalloc_array(eh->e_phnum, sizeof(*ph), GFP_KERNEL);
	if (!ph)
		return -ENOMEM;
	pos = eh->e_phoff;
	ret = kernel_read(bprm->file, ph, eh->e_phnum * sizeof(*ph), &pos);
	if (ret != eh->e_phnum * sizeof(*ph)) {
		ret = ret < 0 ? ret : -EIO;
		goto out;
	}
	ret = -ENOEXEC;
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type == PT_INTERP)
			goto out;		/* static only */
		if (ph[i].p_type != PT_LOAD)
			continue;
		if (ph[i].p_filesz > ph[i].p_memsz)
			goto out;
		if (ph[i].p_vaddr >= ATRANS_WINDOW && ph[i].p_vaddr < ATRANS_WEND &&
		    ph[i].p_memsz <= ATRANS_WEND - ph[i].p_vaddr) {
			lo = min_t(unsigned long, lo, ph[i].p_vaddr);
			hi = max_t(unsigned long, hi, ph[i].p_vaddr + ph[i].p_memsz);
		} else if (ph[i].p_vaddr >= ATRANS_TEXT && ph[i].p_vaddr < ATRANS_TEND &&
			   ph[i].p_memsz <= ATRANS_TEND - ph[i].p_vaddr) {
			/* v3 shared text: read-only, no bss */
			if ((ph[i].p_flags & PF_W) || ph[i].p_filesz != ph[i].p_memsz) {
				pr_warn_ratelimited("%s: writable or bss segment at %#x in the ATRANS text window\n",
						    bprm->filename, ph[i].p_vaddr);
				ret = -EINVAL;
				goto out;
			}
			tlo = min_t(unsigned long, tlo, ph[i].p_vaddr);
			thi = max_t(unsigned long, thi, ph[i].p_vaddr + ph[i].p_memsz);
		} else {
			goto out;		/* not ours (FDPIC binaries link at 0) */
		}
		if (eh->e_phoff >= ph[i].p_offset &&
		    eh->e_phoff + eh->e_phnum * sizeof(*ph) <= ph[i].p_offset + ph[i].p_filesz)
			phdr_va = ph[i].p_vaddr + eh->e_phoff - ph[i].p_offset;
	}
	if (!hi && !thi)
		goto out;
	if (!hi)			/* text only: default 64 KiB stack */
		lo = hi = ATRANS_WINDOW + 0x10000;
	if (lo < ATRANS_WINDOW + ATRANS_MINSTACK) {
		pr_warn_ratelimited("%s: ATRANS program linked at %#lx: the stack goes below the image, link at 0x11020000\n",
				    bprm->filename, lo);
		ret = -EINVAL;
		goto out;
	}
	size = min(PAGE_ALIGN(hi) - ATRANS_WINDOW + ATRANS_HEAP0, ATRANS_MAX);

	ret = -ENOMEM;
	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		goto out;
	a->stack_top = lo;
	bprm->mm->context.atrans = a;		/* freed with bprm->mm on failure */
	if (thi) {
		t = text_get(bprm, ph, eh->e_phnum, tlo, thi);
		if (IS_ERR(t)) {
			ret = PTR_ERR(t);
			goto out;
		}
		a->text = t;			/* put with bprm->mm on failure */
	}
	kva = block_alloc(size);
	if (!kva)
		goto out;
	set_block(bprm->mm, kva, size);

	ret = begin_new_exec(bprm);		/* switches to bprm->mm: window = block */
	if (ret)
		goto out;
	set_personality(PER_LINUX_32BIT);
	setup_new_exec(bprm);
	mm = current->mm;

	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD || ph[i].p_vaddr >= ATRANS_TEXT)
			continue;	/* the text is in place already */
		if (ph[i].p_filesz) {
			pos = ph[i].p_offset;
			ret = kernel_read(bprm->file, (void *)ph[i].p_vaddr, ph[i].p_filesz, &pos);
			if (ret != ph[i].p_filesz) {
				ret = ret < 0 ? ret : -EIO;
				goto out;
			}
		}
		memset((void *)(ph[i].p_vaddr + ph[i].p_filesz), 0, ph[i].p_memsz - ph[i].p_filesz);
		if (ph[i].p_flags & PF_X) {
			mm->start_code = ph[i].p_vaddr;
			mm->end_code = ph[i].p_vaddr + ph[i].p_filesz;
		} else {
			mm->start_data = ph[i].p_vaddr;
			mm->end_data = ph[i].p_vaddr + ph[i].p_filesz;
		}
	}
	if (thi) {
		mm->start_code = tlo;
		mm->end_code = thi;
	}
	/* Text and data are in their blocks now: drop the clean page cache the
	 * reads left (as POSIX_FADV_DONTNEED would). Those pages would sit
	 * scattered in free memory, splitting the runs the next block needs. */
	invalidate_mapping_pages(bprm->file->f_mapping, 0, -1);
	mm->start_brk = mm->brk = PAGE_ALIGN(hi);
	set_end_brk(mm);
	set_binfmt(&atrans_format);

	mm->start_stack = lo;
	ret = transfer_args_to_stack(bprm, &mm->start_stack);
	if (!ret)
		ret = atrans_tables(bprm, mm, eh, ph, phdr_va);
	if (ret)
		goto out;
	rp2350_atrans_set_tls(0);		/* flush_thread cleared the thread pointer */
	st.execs++;
	finalize_exec(bprm);
	start_thread(current_pt_regs(), (eh->e_entry | 1), mm->start_stack);	/* Thumb */
	ret = 0;
out:
	kfree(ph);
	return ret;
}

static struct linux_binfmt atrans_format = {
	.module = THIS_MODULE,
	.load_binary = load_atrans_binary,
};

static int atrans_show(struct seq_file *m, void *v)
{
	struct task_struct *p;

	seq_printf(m, "window %#lx, up to %lu KiB per process, loaded %#lx\n", ATRANS_WINDOW,
		   ATRANS_MAX >> 10, loaded);
	seq_printf(m, "mpu %s\n", mpu_size ? "on (window only for user mode)" : "off");
	seq_printf(m, "execs %lu forks %lu exits %lu live %lu (%lu KiB) nomem %lu badsp %lu cache drops %lu\n",
		   st.execs, st.forks, st.exits, st.live, st.live_bytes >> 10, st.nomem, st.badsp,
		   st.drops);
	seq_printf(m, "runs refused to keep the kernel's reserve %lu, taken down to low %lu, swap waits given up %lu\n",
		   st.reserve, st.lowres, st.gaveup);
	seq_printf(m, "mmaps %lu munmaps %lu grows %lu (in place %lu) shrinks %lu\n",
		   st.mmaps, st.munmaps, st.grows + st.grows_inplace, st.grows_inplace, st.shrinks);
	seq_printf(m, "switches %lu, avg %lu cycles, max %lu\n", st.switches,
		   st.switches ? st.switch_cycles / st.switches : 0, st.switch_max);
	seq_printf(m, "fork copies %lu, avg %lu cycles, max %lu, avg %lu bytes\n", st.forks,
		   st.forks ? st.fork_cycles / st.forks : 0, st.fork_max,
		   st.forks ? st.fork_bytes / st.forks : 0);
	seq_printf(m, "grow copies %lu, avg %lu cycles, max %lu, avg %lu bytes\n", st.grows,
		   st.grows ? st.grow_cycles / st.grows : 0, st.grow_max,
		   st.grows ? st.grow_bytes / st.grows : 0);
	seq_printf(m, "texts %lu (%lu KiB), loads %lu, shared hits %lu\n", st.texts,
		   st.text_bytes >> 10, st.text_loads, st.text_hits);
	atrans_pool_show(m);
	mutex_lock(&text_lock);
	{
		struct atrans_text *t;

		list_for_each_entry(t, &texts, list)
			seq_printf(m, "  text inode %lu: block %08lx %5lu KiB, %d users\n",
				   t->inode->i_ino, t->kva - CONFIG_DRAM_BASE, t->size >> 10, t->users);
	}
	mutex_unlock(&text_lock);
	seq_puts(m, "  pid comm             block     KiB   brk        stack_top  text KiB\n");
	rcu_read_lock();
	for_each_process(p) {
		struct mm_struct *mm = p->mm;

		if (mm && mm->context.atrans_kva)
			seq_printf(m, "%5d %-16s %08lx %5lu   %08lx   %08lx   %5lu\n", task_pid_nr(p),
				   p->comm, mm->context.atrans_kva - CONFIG_DRAM_BASE,
				   mm->context.atrans_size >> 10, mm->brk,
				   mm->context.atrans ? mm->context.atrans->stack_top : 0,
				   mm_tsize(mm) >> 10);
	}
	rcu_read_unlock();
	return 0;
}

static int __init rp2350_atrans_init(void)
{
	/* DWT cycle counter, for the statistics */
	writel_relaxed(readl_relaxed((void __iomem *)0xe000edfc) | BIT(24), (void __iomem *)0xe000edfc);
	writel_relaxed(readl_relaxed((void __iomem *)0xe0001000) | 1, (void __iomem *)0xe0001000);
	atrans_map(0, 0);	/* nothing in the window until an ATRANS process runs */
	writel_relaxed(0, QMI_ATRANS7);	/* nor in the text window (reset: 4 MiB of PSRAM) */
	if ((readl_relaxed((void __iomem *)0xe000ed90) >> 8 & 0xff) < 3)
		panic("rp2350-atrans: needs 3 MPU regions");
	writel_relaxed(0, M33_MPU_CTRL);
	writel_relaxed(MAIR_NORMAL_WT, M33_MPU_MAIR0);	/* AttrIndx 0 */
	/* region 1: the thread pointer word, read-only (region 0 is set per process) */
	writel_relaxed(1, M33_MPU_RNR);
	writel_relaxed(ATRANS_TP | RBAR_AP_RO_ANY, M33_MPU_RBAR);
	writel_relaxed(((ATRANS_TP + ATRANS_TPSIZE - 1) & ~0x1fUL) | RLAR_EN, M33_MPU_RLAR);
	thread_register_notifier(&atrans_thread_nb);
	insert_binfmt(&atrans_format);	/* ahead of ELF FDPIC */
	proc_create_single("atrans", 0444, NULL, atrans_show);
	atrans_pool_init();
	pr_info("rp2350-atrans: per-process window %#lx, blocks up to %lu KiB\n", ATRANS_WINDOW,
		ATRANS_MAX >> 10);
	return 0;
}
core_initcall(rp2350_atrans_init);
