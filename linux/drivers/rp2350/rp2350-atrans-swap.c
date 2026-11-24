// SPDX-License-Identifier: GPL-2.0
/*
 * Whole-process swapping for ATRANS processes (rp2350-atrans.c), classic
 * Unix style. nommu Linux has no CONFIG_SWAP (it needs page tables), but an
 * ATRANS process lives in one PSRAM block whose window address never
 * changes, so the whole block can go to disk and come back anywhere.
 * See docs/atrans-swap.md.
 *
 * Swap out (when a block for exec, fork or growth can't be found, or free
 * memory is below the "low" threshold): pick an ATRANS process none of
 * whose tasks is current, inside swap code or inside a bio submission
 * (mm_idle: its swap-in couldn't issue I/O) (stopped ones first, then
 * blocked ones, then runnable ones not swapped in during the last second;
 * each by the time they last ran), take its mmap lock (trylock: a process
 * blocked while holding it is skipped; growth holds it throughout), write
 * what it uses (the same notion as fork: the stack from sp, image..brk,
 * the mapped runs) to swap slots, free the block, and set atrans_kva =
 * atrans_size = 0. A swapped-out process has no block, and
 * mm->context.atrans->swap says where its data is. Shared text stays.
 *
 * Swap in: with PREEMPT_NONE a task that is off the CPU resumes by
 * returning from schedule() (it blocked, or was preempted on its way back
 * to user mode) or from preempt_schedule_common() (cond_resched), except a
 * new task, which starts in ret_from_fork (those are never picked). Nothing
 * between the context switch and that return touches user memory. So both
 * call rp2350_atrans_swap_resume() (kernel/sched/core.c, patched by
 * apply.sh), which allocates a block (swapping others out if needed), reads
 * the data back and loads the window, in the task's own context, where it
 * may sleep. Window addresses never change, so the process doesn't notice.
 *
 * Races. Swap-out sets state OUT before its I/O and frees the block only
 * after it. If a task of the victim wakes meanwhile, the hook just marks the
 * swap-out CANCELled and goes on running in its intact block (it doesn't
 * wait: it may hold a lock the I/O needs); swap-out then throws its copy
 * away. A task in swap code carries TIF_ATRANS_SWAP: the hook leaves it
 * alone (no recursion from the sleeps inside swap code) and its process is
 * never picked as a victim (that covers processes mid-fork, mid-exec,
 * mid-growth waiting for swap, and swapping themselves in).
 *
 * Locks: swap_lock (mutex) serialises all swapping and the slot map; it
 * may be taken with an mmap lock held (growth, /proc readers), so with it
 * held, mmap locks are only ever trylocked. swap_spin covers the state
 * changes the hook races with.
 *
 * Cache: the I/O goes through the block's kernel address (struct pages,
 * bios), which nothing else uses for a live block. Before writing a block
 * out, the window's lines are written back (sw_flush), but the window is
 * emptied only once the swap-out has succeeded; after any swap I/O the XIP
 * cache is cleaned and invalidated, so the block's kernel-address lines
 * never outlive the I/O.
 *
 * Window: switch_mm skips the switch back to a process whose mm a kernel
 * thread had borrowed (lazy active_mm), so anything that moved or unloaded
 * its block meanwhile (pool compaction from kswapd, a swap-out from the
 * background worker) would leave it an empty or stale window. The hook
 * reloads current's window whenever it doesn't show current's block.
 *
 * Swap space: a block device or a regular file (its blocks are looked up
 * once with bmap and then written directly, as swapon does) with a Linux
 * swap signature (mkswap): page 0 is the header, slots are pages 1 up to
 * last_page. `echo /dev/xxx > /proc/atrans-swap` starts it, `echo off`
 * swaps everything back in and stops it (EBUSY if that doesn't fit; "off
 * force", or the card gone, kills what doesn't, with a log line). A process
 * that can't come back (read error, or still no room after about 3 s) is
 * killed with a log line, never left waiting. A process's data goes into
 * one contiguous run of slots when there is one.
 */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/sched/clock.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task_stack.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/sysinfo.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/vmstat.h>
#include <linux/workqueue.h>
#include <asm/rp2350-atrans.h>
#include "rp2350-atrans-internal.h"

/* "inside swap code" (see above). ARM uses TIF bits 0-4 and 17-23. */
#define TIF_ATRANS_SWAP	12

/*
 * a->swap states. OUT: being written out (the block still holds it).
 * CANCEL: a task of it woke meanwhile, the copy will be thrown away.
 * SWAPPED: in swap only. LOAD: back in a block, but swapped in by someone
 * else (swapoff): its window gets loaded when it next runs (the hook).
 * DEAD: its data is gone (an I/O error, or no room for it in time):
 * killed, it exits without memory.
 */
enum { SW_OUT = 1, SW_CANCEL, SW_SWAPPED, SW_LOAD, SW_DEAD };

#define SW_MAXEXT	24		/* more ranges than this are merged */

struct swext {
	u32 off;			/* first page, window offset in pages */
	u32 pages;
	u32 slot;
};

struct rp2350_atrans_swap {
	int state;
	unsigned long size;		/* block size to give it back */
	unsigned long pages;		/* pages in swap */
	unsigned int n;
	unsigned long clip;		/* while collecting: the lowest offset kept */
	struct swext ext[SW_MAXEXT];
	pid_t pid;
	char comm[TASK_COMM_LEN];
	unsigned long when;		/* jiffies at swap-out */
};

/* a swap file's blocks on the device: file pages [page, page + pages) */
struct swrun {
	unsigned long page, pages;
	sector_t sector;
};

static DEFINE_MUTEX(swap_lock);
static struct task_struct *swap_owner;	/* holder of swap_lock */
static DEFINE_SPINLOCK(swap_spin);

static struct {
	struct file *file;		/* what was named */
	struct file *bdev_file;		/* the block device, if one was named */
	struct block_device *bdev;	/* where the I/O goes */
	char name[64];
	unsigned long nslots;		/* slot i is page i + 1 of the device or file */
	unsigned long *map;
	unsigned long used, hint;
	struct swrun *runs;		/* a swap file: where its pages are */
	unsigned int nruns;
} sw;

static bool draining;			/* swapoff: no new swap-outs */

/* the last rp2350_atrans_swap_reclaim(), for block_alloc's failure line:
 * what reclaim_for and reclaim_one did, and the processes passed over */
static struct {
	int rf, none, ro;		/* -1: not run */
	unsigned int cand, sk_cancel, sk_swapped, sk_busy, sk_exec, sk_new, sk_recent;
} dg;
static unsigned long low_kb;		/* background swap-out below this much free memory */
static unsigned int low_age = 5;	/* ...of processes asleep at least this long (s) */

static struct {
	unsigned long outs, out_pages, ins, in_pages;
	u64 out_ns, in_ns, out_max, in_max;
	unsigned long cancels, ioerr, nospace, nomem, remote, retries, background, targeted, lend;
	unsigned long killed, drained, reloads, lend_waits, lend_timeouts, odd, recancel;
} st;

/* ---- cache ---------------------------------------------------------------------- */

static void sw_flush(void)		/* clean + invalidate the XIP cache */
{
	unsigned long flags;

	local_irq_save(flags);
	xip_flush_all();
	local_irq_restore(flags);
}

/* if the block at kva is in the window, write its lines back and empty the
 * window: then the block can be read through its kernel address */
static void sw_unload(unsigned long kva)
{
	unsigned long flags;

	local_irq_save(flags);
	if (kva && loaded == kva) {
		xip_flush_all();
		atrans_map(0, 0);
	}
	local_irq_restore(flags);
}

/* ---- locking ------------------------------------------------------------------ */

/* Take swap_lock unless current holds it already; returns whether it did. */
static bool swap_get(void)
{
	if (swap_owner == current)
		return false;
	set_thread_flag(TIF_ATRANS_SWAP);
	mutex_lock(&swap_lock);
	swap_owner = current;
	return true;
}

static void swap_put(bool took)
{
	if (!took)
		return;
	swap_owner = NULL;
	mutex_unlock(&swap_lock);
	clear_thread_flag(TIF_ATRANS_SWAP);
}

/* ---- slots ---------------------------------------------------------------------- */

static long slots_get(unsigned long n)
{
	unsigned long s;

	s = bitmap_find_next_zero_area(sw.map, sw.nslots, sw.hint, n, 0);
	if (s >= sw.nslots)
		s = bitmap_find_next_zero_area(sw.map, sw.nslots, 0, n, 0);
	if (s >= sw.nslots)
		return -1;
	bitmap_set(sw.map, s, n);
	sw.used += n;
	sw.hint = s + n;
	return s;
}

static void slots_put(struct rp2350_atrans_swap *s)
{
	unsigned int i;

	for (i = 0; i < s->n; i++)
		if (s->ext[i].slot != U32_MAX) {
			bitmap_clear(sw.map, s->ext[i].slot, s->ext[i].pages);
			sw.used -= s->ext[i].pages;
		}
}

/* one run for all of it if possible (sequential I/O), else one per range */
static int slots_alloc(struct rp2350_atrans_swap *s)
{
	long base = slots_get(s->pages), sl;
	unsigned int i;

	for (i = 0; i < s->n; i++)
		s->ext[i].slot = U32_MAX;
	if (base >= 0) {
		for (i = 0; i < s->n; i++) {
			s->ext[i].slot = base;
			base += s->ext[i].pages;
		}
		return 0;
	}
	for (i = 0; i < s->n; i++) {
		sl = slots_get(s->ext[i].pages);
		if (sl < 0) {
			slots_put(s);
			return -ENOSPC;
		}
		s->ext[i].slot = sl;
	}
	return 0;
}

/* ---- I/O -------------------------------------------------------------------------- */

/* device sector of slot `slot`, and how many pages from there are contiguous */
static sector_t slot_sector(unsigned long slot, unsigned long *run)
{
	unsigned long page = slot + 1;	/* page 0: the swap header */
	unsigned int lo = 0, hi = sw.nruns, mid;

	if (!sw.runs) {
		*run = ULONG_MAX;
		return (sector_t)page << (PAGE_SHIFT - SECTOR_SHIFT);
	}
	while (lo + 1 < hi) {		/* the last run starting at or before page */
		mid = (lo + hi) / 2;
		if (sw.runs[mid].page <= page)
			lo = mid;
		else
			hi = mid;
	}
	*run = sw.runs[lo].page + sw.runs[lo].pages - page;
	return sw.runs[lo].sector + ((sector_t)(page - sw.runs[lo].page) << (PAGE_SHIFT - SECTOR_SHIFT));
}

struct swio {
	atomic_t pending;
	struct completion done;
	blk_status_t err;
};

static void swio_end(struct bio *bio)
{
	struct swio *io = bio->bi_private;

	if (bio->bi_status)
		io->err = bio->bi_status;
	bio_put(bio);
	if (atomic_dec_and_test(&io->pending))
		complete(&io->done);
}

/* Read or write `pages` pages at kernel address kva from or to slot `slot`
 * on: bios of up to 1 MiB, all in flight at once. */
static int swap_rw(blk_opf_t op, unsigned long kva, unsigned long slot, unsigned long pages)
{
	struct swio io = { .err = BLK_STS_OK };
	struct blk_plug plug;

	atomic_set(&io.pending, 1);
	init_completion(&io.done);
	blk_start_plug(&plug);
	while (pages) {
		unsigned long run, n, i;
		sector_t sec = slot_sector(slot, &run);
		struct bio *bio;

		n = min3(pages, run, (unsigned long)BIO_MAX_VECS);
		bio = bio_alloc(sw.bdev, n, op, GFP_NOIO);
		bio->bi_iter.bi_sector = sec;
		bio->bi_end_io = swio_end;
		bio->bi_private = &io;
		for (i = 0; i < n; i++, kva += PAGE_SIZE)
			__bio_add_page(bio, virt_to_page((void *)kva), PAGE_SIZE, 0);
		atomic_inc(&io.pending);
		submit_bio(bio);
		slot += n;
		pages -= n;
	}
	blk_finish_plug(&plug);
	if (!atomic_dec_and_test(&io.pending))
		wait_for_completion_io(&io.done);
	return io.err ? -EIO : 0;
}

static int swap_rw_all(struct rp2350_atrans_swap *s, blk_opf_t op, unsigned long kva)
{
	unsigned int i;
	int err = 0;

	for (i = 0; i < s->n && !err; i++)
		err = swap_rw(op, kva + ((unsigned long)s->ext[i].off << PAGE_SHIFT), s->ext[i].slot,
			      s->ext[i].pages);
	return err;
}

/* ---- swap out --------------------------------------------------------------------- */

static void add_ext(unsigned long off, unsigned long len, void *arg)
{
	struct rp2350_atrans_swap *s = arg;
	unsigned long first = off >> PAGE_SHIFT, end = PAGE_ALIGN(off + len) >> PAGE_SHIFT;
	struct swext *e = s->n ? &s->ext[s->n - 1] : NULL;

	/* the first range (stack, image, heap) is clipped at the stack page
	 * holding sp: atrans_for_each_used() can't tell that only one task
	 * uses mm (swap-out holds a reference), so it gives all of the stack */
	if (!s->n && off < s->clip) {
		len = off + len > s->clip ? off + len - s->clip : 0;
		off = s->clip;
		first = off >> PAGE_SHIFT;
	}
	if (!len)
		return;
	/* ranges come in ascending order; overlapping or adjacent ones join,
	 * and with no room left the last one stretches over the gap */
	if (e && (first <= e->off + e->pages || s->n == SW_MAXEXT)) {
		if (end > e->off + e->pages)
			e->pages = end - e->off;
		return;
	}
	s->ext[s->n].off = first;
	s->ext[s->n].pages = end - first;
	s->n++;
}

/* processes swapped in lately: not swapped out again while runnable for
 * SW_RESIDENT jiffies, or runnable processes would just take turns */
#define SW_RESIDENT	HZ
static struct { struct mm_struct *mm; unsigned long t; } recent[8];
static unsigned int recent_i;

static bool recently_in(struct mm_struct *mm)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(recent); i++)
		if (recent[i].mm == mm && time_before(jiffies, recent[i].t + SW_RESIDENT))
			return true;
	return false;
}

/*
 * Processes whose swap-out was just cancelled (a task of theirs woke in the
 * middle of the write): not tried again for SW_RECANCEL by the shrinker
 * and the background worker (reclaim_one). The process is awake, so
 * writing it out again would be cancelled again, and the two would keep
 * writing it out for nothing. reclaim_for, for a process waiting for a
 * block, still counts them: without them it can find no way to the room
 * ("none") when one of them stands in the way, and the waiting exec, fork
 * or growth fails. Its retries are bounded (block_alloc's BLOCK_WAIT).
 * swap_lock held.
 */
#define SW_RECANCEL	(2 * HZ)
static struct { struct mm_struct *mm; unsigned long t; } recancel_mm[8];
static unsigned int recancel_i;

static bool just_cancelled(struct mm_struct *mm)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(recancel_mm); i++)
		if (recancel_mm[i].mm == mm && time_before(jiffies, recancel_mm[i].t + SW_RECANCEL)) {
			st.recancel++;
			dg.sk_cancel++;
			return true;
		}
	return false;
}

/* sort key classes: the lowest goes first */
enum { K_STOPPED, K_BLOCKED, K_RUNNABLE };
#define K_SHIFT	62

/*
 * Whether mm can be swapped out: no task of it is current or in swap code,
 * and each one is off the CPU in a way that resumes through a swap-in hook:
 * blocked or preempted (both return through schedule() or, for
 * cond_resched, preempt_schedule_common(); with PREEMPT_NONE there is no
 * other way off the CPU). A task that has never run would start in
 * ret_from_fork instead, so it rules mm out, and so does one in exec. Also
 * reports the tasks'
 * number, the user sp of one, and a sort key: stopped first, then blocked,
 * then runnable (not swapped in during the last second), each by the time
 * they last ran. rcu_read_lock held.
 */
static bool mm_idle(struct mm_struct *mm, unsigned int *ntasks, unsigned long *sp, u64 *key,
		    struct task_struct **who)
{
	struct task_struct *g, *t;
	int class = K_STOPPED;
	u64 last = 0;

	*ntasks = 0;
	for_each_process_thread(g, t) {
		if (t->mm != mm)
			continue;
		/* Inside a bio submission (bio_list: e.g. preempted at the
		 * cond_resched() in mtd_queue_rq): its swap-in would submit
		 * the swap reads from there, and they would only be queued on
		 * its bio_list behind the submission it is in the middle of,
		 * never issued, with swap_lock held for ever. A plug alone
		 * can't do that (swap_rw's wait_for_completion_io() flushes
		 * it); a task with one is skipped too, as one in the middle
		 * of block I/O, which costs only a later victim. */
		if (t == current || test_ti_thread_flag(task_thread_info(t), TIF_ATRANS_SWAP) ||
		    t->bio_list || t->plug) {
			dg.sk_busy++;
			return false;
		}
		/* exec past exec_mmap: the new mm has its block but no brk yet
		 * (its "used" range wrapped round: 4 GB, "no space"), and the
		 * loader is still filling it through the window */
		if (t->in_execve) {
			dg.sk_exec++;
			return false;
		}
		if (task_is_running(t)) {
			if (!t->se.sum_exec_runtime) {
				dg.sk_new++;
				return false;		/* not started yet */
			}
			class = K_RUNNABLE;
		} else if (!task_is_stopped_or_traced(t)) {
			class = max(class, K_BLOCKED);
		}
		last = max(last, t->se.exec_start);
		*sp = task_pt_regs(t)->ARM_sp;
		*who = t;
		(*ntasks)++;
	}
	if (class == K_RUNNABLE && recently_in(mm)) {
		dg.sk_recent++;
		return false;
	}
	*key = (u64)class << K_SHIFT | (last >> 2);
	return *ntasks != 0;
}

/* The best victim (mmget held), or NULL. `skip`: mms already tried. */
static struct mm_struct *pick_victim(struct mm_struct **skip, int nskip, unsigned long min_idle_ns)
{
	struct task_struct *p, *who, *best_task = NULL;
	struct mm_struct *mm, *best = NULL;
	u64 key, best_key = U64_MAX, now = sched_clock();
	unsigned long sp;
	unsigned int n;
	int i;

	rcu_read_lock();
	for_each_process(p) {
		mm = READ_ONCE(p->mm);
		if (mm && mm->context.atrans && mm->context.atrans->swap) {
			dg.sk_swapped++;
			continue;
		}
		if (!mm || mm == current->mm || !mm->context.atrans || !mm->context.atrans_kva)
			continue;
		for (i = 0; i < nskip && skip[i] != mm; i++)
			;
		if (i < nskip || just_cancelled(mm) || !mm_idle(mm, &n, &sp, &key, &who))
			continue;
		dg.cand++;
		/* background: only processes asleep (or stopped) long enough */
		if (min_idle_ns && ((key >> K_SHIFT) == K_RUNNABLE ||
				    ((key >> K_SHIFT) == K_BLOCKED &&
				     now - (key << 2) < min_idle_ns)))
			continue;
		if (key < best_key) {
			best_key = key;
			best = mm;
			best_task = p;
		}
	}
	if (best) {
		task_lock(best_task);
		if (best_task->mm != best || !mmget_not_zero(best))
			best = NULL;
		task_unlock(best_task);
	}
	rcu_read_unlock();
	return best;
}

/* Swap mm out (swap_lock held, mm referenced). 0 or an error: it stays. */
static int swap_out(struct mm_struct *mm)
{
	struct rp2350_atrans_mm *a = mm->context.atrans;
	struct rp2350_atrans_swap *s;
	struct task_struct *who;
	unsigned long kva, size, sp = 0, lo = ATRANS_WINDOW;
	unsigned int ntasks, i;
	bool cancelled;
	u64 key, t0, dt;
	int err;

	if (!down_write_trylock(&mm->mmap_lock))	/* (no mmap_write_trylock in 6.12) */
		return -EBUSY;
	err = -EBUSY;
	kva = mm->context.atrans_kva;
	size = mm->context.atrans_size;
	if (!a || !kva || a->swap)
		goto unlock;
	s = kzalloc(sizeof(*s), GFP_NOIO);
	if (!s) {
		err = -ENOMEM;
		goto unlock;
	}
	rcu_read_lock();
	if (!mm_idle(mm, &ntasks, &sp, &key, &who)) {
		rcu_read_unlock();
		kfree(s);
		goto unlock;
	}
	s->pid = task_tgid_nr(who);
	get_task_comm(s->comm, who);
	rcu_read_unlock();
	/* the stack from sp, as fork; all of it if several tasks share mm */
	if (ntasks == 1 && sp >= ATRANS_WINDOW && sp < a->stack_top)
		lo = sp & PAGE_MASK;
	s->clip = lo - ATRANS_WINDOW;
	atrans_for_each_used(mm, 0, add_ext, s);	/* sp 0: all of the stack */
	for (i = 0; i < s->n; i++)
		s->pages += s->ext[i].pages;
	s->size = size;
	if (!s->pages || s->pages > ATRANS_PAGES ||
	    s->ext[s->n - 1].off + s->ext[s->n - 1].pages > ATRANS_PAGES) {
		pr_warn_ratelimited("rp2350-atrans-swap: %s[%d]: odd layout (%lu pages in %u ranges, brk %#lx, %lu KiB block), not swapped\n",
				    s->comm, s->pid, s->pages, s->n, mm->brk, size >> 10);
		st.odd++;
		kfree(s);
		err = -EINVAL;
		goto unlock;
	}
	err = slots_alloc(s);
	if (err) {
		st.nospace++;
		kfree(s);
		goto unlock;
	}
	/* nothing sleeps between mm_idle() and here: no task of mm has run */
	spin_lock_irq(&swap_spin);
	s->state = SW_OUT;
	a->swap = s;
	spin_unlock_irq(&swap_spin);

	/*
	 * Window lines to PSRAM, but leave ATRANS4 as it is: if a task of
	 * the victim wakes and cancels this, it goes on in this block, and it
	 * may be switched in without a switch_mm (a kernel thread, the
	 * background worker say, had borrowed its mm), so emptying the window
	 * here would leave it running with none. Nothing writes through the
	 * window meanwhile unless the victim runs, and then the copy is
	 * thrown away.
	 */
	sw_flush();
	t0 = ktime_get_ns();
	err = swap_rw_all(s, REQ_OP_WRITE, kva);
	sw_flush();			/* drop the lines the I/O left under kva */
	dt = ktime_get_ns() - t0;

	spin_lock_irq(&swap_spin);
	cancelled = err || s->state == SW_CANCEL;
	if (cancelled) {
		a->swap = NULL;
	} else {
		s->state = SW_SWAPPED;
		s->when = jiffies;
		set_block(mm, 0, 0);
		mm->total_vm = size >> PAGE_SHIFT;	/* its VSZ */
		atrans_rss_set(mm, MM_SWAPENTS, s->pages);
	}
	spin_unlock_irq(&swap_spin);
	if (cancelled) {
		if (err) {
			st.ioerr++;
		} else {
			st.cancels++;
			recancel_mm[recancel_i].mm = mm;
			recancel_mm[recancel_i].t = jiffies;
			recancel_i = (recancel_i + 1) % ARRAY_SIZE(recancel_mm);
		}
		slots_put(s);
		kfree(s);
		err = err ?: -EAGAIN;
		goto unlock;
	}
	sw_unload(kva);		/* swapped out: the window must not show the freed block */
	block_free(kva, size);
	count_vm_events(PSWPOUT, s->pages);
	st.outs++;
	st.out_pages += s->pages;
	st.out_ns += dt;
	st.out_max = max(st.out_max, dt);
	err = 0;
unlock:
	mmap_write_unlock(mm);
	return err;
}

/* Swap one process out. swap_lock held. min_idle_ns: only processes that
 * have slept that long (0: any). */
static bool reclaim_one(unsigned long min_idle_ns)
{
	struct mm_struct *skip[8], *mm;
	int nskip = 0, err;

	while (nskip < ARRAY_SIZE(skip)) {
		mm = pick_victim(skip, nskip, min_idle_ns);
		if (!mm)
			break;
		skip[nskip++] = mm;
		err = swap_out(mm);
		mmput(mm);		/* may be the last: exit_mmap, swap_exit */
		if (!err)
			return true;
	}
	return false;
}

#define SW_MAXCAND	24

struct cand {
	struct task_struct *task;	/* referenced */
	struct mm_struct *mm;		/* (identity only) */
	unsigned long pfn, pages;	/* its block when looked at */
	unsigned int in;		/* pages of it in the window */
};

/*
 * The ATRANS pool's way (rp2350-atrans-pool.c): compaction there makes any
 * free space usable, so swapping out candidates whose blocks are in the pool
 * until it has `pages` free in all also works. Taken (c[i].in set) when it
 * costs fewer pages than the cheapest contiguous window.
 */
static bool rp2350_pool_option(struct cand *c, unsigned int nc, unsigned long pages,
			       unsigned long bestcost)
{
	unsigned long kva[SW_MAXCAND], np[SW_MAXCAND], pc;
	bool pick[SW_MAXCAND] = { false };
	unsigned int i;

	for (i = 0; i < nc; i++) {
		kva[i] = (unsigned long)page_address(pfn_to_page(c[i].pfn));
		np[i] = c[i].pages;
	}
	pc = atrans_pool_pick(pages, kva, np, nc, pick);
	if (!pc || pc == ULONG_MAX || pc >= bestcost)
		return false;
	for (i = 0; i < nc; i++)
		c[i].in = pick[i];
	return true;
}

/*
 * Make room for a block of `pages` contiguous pages. Freeing total memory
 * is not enough: the block has to be one physically contiguous run. So
 * classify every page of the zone as free (a free buddy block), part of
 * the block of an idle process (a swap-out candidate, as pick_victim), or
 * anything else (kernel memory, page cache, FDPIC programs, shared text,
 * running processes: pinned). The best window is the run of `pages` pages
 * without a pinned page whose overlapping candidates have the fewest pages
 * between them; swap those out. Returns the number swapped out; 0 with
 * *none set when no such window exists (swapping can't help), 0 with
 * *none clear when the window is free already (a watermark said no: free
 * memory elsewhere instead).
 */
static int reclaim_for(unsigned long pages, bool *none)
{
	struct zone *zone = NULL, *z;
	unsigned long start, end, nr, pfn, flags, e, cost = 0, best = ULONG_MAX, bestcost = ULONG_MAX;
	unsigned int cnt[SW_MAXCAND] = { 0 }, pinned = 0, nc = 0, i, done = 0;
	struct task_struct *p, *who;
	struct mm_struct *mm;
	struct cand *c;
	unsigned long sp;
	unsigned int n;
	u8 *cls;			/* 0: pinned, 1: free, 2 + i: candidate i */
	u64 key;

	*none = true;
	for_each_populated_zone(z) {
		zone = z;		/* nommu, flat: one zone */
		break;
	}
	if (!zone)
		return 0;
	start = zone->zone_start_pfn;
	end = zone_end_pfn(zone);
	nr = end - start;
	if (pages > nr)
		return 0;
	cls = kzalloc(nr, GFP_NOIO);
	c = kcalloc(SW_MAXCAND, sizeof(*c), GFP_NOIO);
	if (!cls || !c) {
		*none = false;
		goto out;
	}

	rcu_read_lock();
	for_each_process(p) {
		mm = READ_ONCE(p->mm);
		if (nc == SW_MAXCAND)
			break;
		if (mm && mm->context.atrans && mm->context.atrans->swap) {
			dg.sk_swapped++;
			continue;
		}
		if (!mm || mm == current->mm || !mm->context.atrans || !mm->context.atrans_kva)
			continue;
		for (i = 0; i < nc && c[i].mm != mm; i++)
			;
		if (i < nc || !mm_idle(mm, &n, &sp, &key, &who))
			continue;
		dg.cand++;
		get_task_struct(p);
		c[nc].task = p;
		c[nc].mm = mm;
		c[nc].pfn = virt_to_pfn((void *)mm->context.atrans_kva);
		c[nc].pages = mm->context.atrans_size >> PAGE_SHIFT;
		nc++;
	}
	rcu_read_unlock();

	drain_local_pages(zone);
	spin_lock_irqsave(&zone->lock, flags);
	for (pfn = start; pfn < end; ) {
		struct page *pg = pfn_to_page(pfn);
		unsigned long k, o;

		if (!PageBuddy(pg)) {
			pfn++;
			continue;
		}
		o = 1UL << page_private(pg);	/* buddy_order() */
		for (k = 0; k < o && pfn + k < end; k++)
			cls[pfn + k - start] = 1;
		pfn += o;
	}
	spin_unlock_irqrestore(&zone->lock, flags);
	atrans_pool_mark_free(cls, start, end);		/* the ATRANS pool's free pages */
	for (i = 0; i < nc; i++)
		for (pfn = c[i].pfn; pfn < c[i].pfn + c[i].pages; pfn++)
			if (pfn >= start && pfn < end)
				cls[pfn - start] = 2 + i;

	/* sliding window: no pinned page, least candidate pages to swap out */
	for (e = 0; e < nr; e++) {
		u8 a = cls[e];

		if (!a)
			pinned++;
		else if (a >= 2 && !cnt[a - 2]++)
			cost += c[a - 2].pages;
		if (e >= pages) {
			u8 r = cls[e - pages];

			if (!r)
				pinned--;
			else if (r >= 2 && !--cnt[r - 2])
				cost -= c[r - 2].pages;
		}
		if (e + 1 >= pages && !pinned && cost < bestcost) {
			bestcost = cost;
			best = e + 1 - pages;
		}
	}
	if (rp2350_pool_option(c, nc, pages, bestcost)) {
		*none = false;		/* the ATRANS pool, compacted, is cheaper */
		goto swap;
	}
	if (best == ULONG_MAX)
		goto out;		/* swapping can't make such a run */
	*none = false;
	for (e = best; e < best + pages; e++)
		if (cls[e] >= 2)
			c[cls[e] - 2].in++;
swap:
	for (i = 0; i < nc; i++) {
		if (!c[i].in)
			continue;
		mm = NULL;
		task_lock(c[i].task);
		if (c[i].task->mm == c[i].mm && mmget_not_zero(c[i].mm))
			mm = c[i].mm;
		task_unlock(c[i].task);
		if (!mm)
			continue;
		if (!swap_out(mm))
			done++;
		mmput(mm);		/* may be the last: exit_mmap, swap_exit */
	}
	if (done)
		st.targeted++;
out:
	if (c)
		for (i = 0; i < nc; i++)
			put_task_struct(c[i].task);
	kfree(c);
	kfree(cls);
	return done;
}

/*
 * block_alloc() found no room for a block of `size` bytes: swap out the
 * idle processes whose blocks stand in the way of the cheapest contiguous
 * run (reclaim_for), or, if that run is free already but a watermark
 * refused it, the process that ran least recently. block_alloc retries
 * while this returns true. Called from exec, fork, growth and swap-in, all
 * in process context.
 */
bool rp2350_atrans_swap_reclaim(unsigned long size)
{
	bool took, ok = false, none;

	if (!READ_ONCE(sw.bdev) || READ_ONCE(draining))
		return false;
	took = swap_get();
	memset(&dg, 0, sizeof(dg));
	dg.rf = dg.ro = dg.none = -1;
	if (sw.bdev && !draining) {
		dg.rf = reclaim_for(size >> PAGE_SHIFT, &none);
		dg.none = none;
		ok = dg.rf > 0;
		if (!ok && !none)
			ok = (dg.ro = reclaim_one(0));
	}
	if (!ok)
		st.nomem++;
	swap_put(took);
	return ok;
}

int rp2350_atrans_swap_diag(char *buf, int len)
{
	if (!READ_ONCE(sw.bdev))
		return scnprintf(buf, len, "no swap");
	return scnprintf(buf, len, "reclaim_for %d (none %d), reclaim_one %d; %u candidates, passed over: %u swapped out, %u just cancelled, %u busy, %u in exec, %u new, %u just swapped in",
			 dg.rf, dg.none, dg.ro, dg.cand, dg.sk_swapped, dg.sk_cancel, dg.sk_busy,
			 dg.sk_exec, dg.sk_new, dg.sk_recent);
}

/* ---- swap in ---------------------------------------------------------------------- */

static unsigned long free_kb(void)
{
	return global_zone_page_state(NR_FREE_PAGES) << (PAGE_SHIFT - 10);
}

/*
 * mm's data can't come back: mark it DEAD (its slots go back now), log why,
 * and kill every task using it. They exit without memory: access_ok fails
 * for a process with no block, so the exit path can't touch the window.
 * swap_lock held.
 */
static void swap_kill(struct mm_struct *mm, const char *why)
{
	struct rp2350_atrans_swap *s = mm->context.atrans->swap;
	struct task_struct *g, *t;
	unsigned long flags;

	pr_warn("rp2350-atrans-swap: %s[%d] (%lu KiB in swap) killed: %s (%lu KiB free, swap %lu of %lu KiB used)\n",
		s->comm, s->pid, s->pages << (PAGE_SHIFT - 10), why, free_kb(),
		sw.used << (PAGE_SHIFT - 10), sw.nslots << (PAGE_SHIFT - 10));
	slots_put(s);
	s->n = 0;
	s->pages = 0;
	spin_lock_irqsave(&swap_spin, flags);
	s->state = SW_DEAD;
	spin_unlock_irqrestore(&swap_spin, flags);
	atrans_rss_set(mm, MM_SWAPENTS, 0);
	rcu_read_lock();
	for_each_process_thread(g, t)
		if (t->mm == mm)
			send_sig(SIGKILL, t, 1);
	rcu_read_unlock();
	st.killed++;
}

/*
 * Bring mm back (swap_lock held): a block (block_alloc may swap others
 * out), the data read into it. For current, the window is loaded at once;
 * for another process (swapoff) the state becomes LOAD and its own hook
 * loads its window when it next runs: it may be switched in without a
 * switch_mm (same mm as a kernel thread had borrowed), so this can't be
 * left to switch_mm. 0: resident now; -ENOMEM: no room; -EIO: its data is
 * lost (the caller kills it).
 */
static int swap_in(struct mm_struct *mm)
{
	struct rp2350_atrans_mm *a = mm->context.atrans;
	struct rp2350_atrans_swap *s = a ? a->swap : NULL;
	bool self = mm == current->mm;
	unsigned long kva, flags;
	u64 t0, dt;
	int err;

	if (!s || s->state != SW_SWAPPED)
		return 0;
	kva = block_alloc(s->size);	/* may swap others out */
	if (!kva)
		return -ENOMEM;
	t0 = ktime_get_ns();
	err = swap_rw_all(s, REQ_OP_READ, kva);
	sw_flush();		/* the data to PSRAM, no lines left under kva */
	dt = ktime_get_ns() - t0;
	if (err) {
		st.ioerr++;
		block_free(kva, s->size);
		return -EIO;
	}
	slots_put(s);
	s->n = 0;
	spin_lock_irqsave(&swap_spin, flags);
	set_block(mm, kva, s->size);
	atrans_rss_set(mm, MM_SWAPENTS, 0);
	if (self)
		a->swap = NULL;
	else
		s->state = SW_LOAD;
	spin_unlock_irqrestore(&swap_spin, flags);
	recent[recent_i].mm = mm;
	recent[recent_i].t = jiffies;
	recent_i = (recent_i + 1) % ARRAY_SIZE(recent);
	count_vm_events(PSWPIN, s->pages);
	st.ins++;
	st.in_pages += s->pages;
	st.in_ns += dt;
	st.in_max = max(st.in_max, dt);
	if (self) {
		kfree(s);
		ensure_loaded(mm);
	}
	return 0;
}

/* how long a task waits for room to come back before it is killed */
#define SW_WAIT_TRIES	30		/* x 100 ms */

/*
 * End of schedule() and preempt_schedule_common() (kernel/sched/core.c): if
 * current's process is swapped out, bring it back before anything touches
 * its memory; if it is being swapped out, cancel that and go on; if
 * swapoff brought it back, load its window.
 *
 * When there is no room: block_alloc has already swapped out every idle
 * process in the way. Wait a little (others may exit, or stop running),
 * releasing swap_lock meanwhile, and then kill it with a log line, rather
 * than waiting for ever: it may hold locks other processes need.
 */
void rp2350_atrans_swap_resume(void)
{
	struct mm_struct *mm = current->mm;
	struct rp2350_atrans_mm *a;
	struct rp2350_atrans_swap *s;
	int state = 0, err, tries;
	unsigned long flags;

	if (!mm)
		return;
	/*
	 * The window must show current's block. switch_mm skips a switch to
	 * the mm a kernel thread had borrowed (prev->active_mm == next->mm),
	 * and if that kernel thread moved the block meanwhile (the ATRANS
	 * pool's shrinker compacting, from kswapd say) or unloaded it, the
	 * task would come back to an empty or stale window: a HardFault on
	 * its way to user mode. Every resume but a new task's (whose mm no
	 * one can have borrowed) passes through here, so this covers them.
	 */
	if (mm->context.atrans_kva && READ_ONCE(loaded) != mm->context.atrans_kva) {
		st.reloads++;
		pr_info_ratelimited("rp2350-atrans-swap: %s[%d]: window reloaded (%#lx, was %#lx)\n",
				    current->comm, task_pid_nr(current), mm->context.atrans_kva, loaded);
		ensure_loaded(mm);
	}
	if (test_thread_flag(TIF_ATRANS_SWAP))
		return;
	a = mm->context.atrans;
	if (!a || !READ_ONCE(a->swap))
		return;
	spin_lock_irqsave(&swap_spin, flags);
	s = a->swap;
	if (s) {
		if (s->state == SW_OUT)
			s->state = SW_CANCEL;
		state = s->state;
		if (state == SW_LOAD)
			a->swap = NULL;
	}
	spin_unlock_irqrestore(&swap_spin, flags);
	if (state == SW_LOAD) {
		kfree(s);
		ensure_loaded(mm);
		return;
	}
	if (state == SW_DEAD) {
		send_sig(SIGKILL, current, 1);	/* (it is already) */
		return;
	}
	if (state != SW_SWAPPED)
		return;

	swap_get();
	for (tries = 0;; tries++) {
		/* mm_idle() never picks a task inside a bio submission: its
		 * swap reads would wait on its own bio_list for ever, holding
		 * swap_lock (see there). Kill it rather than hang every
		 * swapper behind it. */
		if (WARN_ON_ONCE(current->bio_list)) {
			if (a->swap && a->swap->state == SW_SWAPPED)
				swap_kill(mm, "swapped out inside a bio submission");
			break;
		}
		err = swap_in(mm);
		if (!err || !a->swap || a->swap->state != SW_SWAPPED)
			break;		/* in, or swapoff dealt with it meanwhile */
		if (err == -EIO) {
			swap_kill(mm, "swap read error");
			break;
		}
		if (tries >= SW_WAIT_TRIES || (fatal_signal_pending(current) && tries >= 3)) {
			swap_kill(mm, "no room to swap it back in");
			break;
		}
		st.retries++;
		if (!tries)
			pr_info("rp2350-atrans-swap: %s[%d] waits for a %lu KiB block (%lu KiB free)\n",
				current->comm, task_pid_nr(current), a->swap->size >> 10, free_kb());
		/* (TIF_ATRANS_SWAP stays set: the sleep doesn't recurse) */
		swap_owner = NULL;
		mutex_unlock(&swap_lock);
		schedule_timeout_uninterruptible(HZ / 10);
		mutex_lock(&swap_lock);
		swap_owner = current;
	}
	/* swapoff may have brought it back while it waited */
	spin_lock_irqsave(&swap_spin, flags);
	s = a->swap;
	state = s ? s->state : 0;
	if (state == SW_LOAD)
		a->swap = NULL;
	spin_unlock_irqrestore(&swap_spin, flags);
	if (state == SW_LOAD) {
		kfree(s);
		ensure_loaded(mm);
	}
	swap_put(true);
}

/*
 * swapoff (swap_lock held, draining set): bring every swapped-out process
 * back. One whose data can't be read (the card is gone) is killed with a
 * log line. One that doesn't fit is killed too, with the reason
 * `force_why` ("off force", or the device is gone), if that is set;
 * otherwise swapoff fails with EBUSY and swap stays on. Returns the number
 * that are still swapped out.
 */
static int swap_drain(const char *force_why)
{
	struct task_struct *p, *who;
	struct mm_struct *mm, *skip[16];
	unsigned int nskip = 0, i, left = 0;
	int err;

	for (;;) {
		who = NULL;
		mm = NULL;
		rcu_read_lock();
		for_each_process(p) {
			struct mm_struct *m = READ_ONCE(p->mm);
			struct rp2350_atrans_swap *s;

			if (!m || !m->context.atrans)
				continue;
			s = m->context.atrans->swap;
			if (!s || s->state != SW_SWAPPED)
				continue;
			for (i = 0; i < nskip && skip[i] != m; i++)
				;
			if (i < nskip)
				continue;
			task_lock(p);
			if (p->mm == m && mmget_not_zero(m))
				mm = m;
			task_unlock(p);
			if (mm)
				break;
		}
		rcu_read_unlock();
		if (!mm)
			break;
		err = swap_in(mm);
		if (!err) {
			st.drained++;
		} else if (err == -EIO) {
			swap_kill(mm, "swapoff, and its data can't be read back");
		} else if (force_why) {
			swap_kill(mm, force_why);
		} else {
			left++;
			if (nskip < ARRAY_SIZE(skip))
				skip[nskip++] = mm;
		}
		mmput(mm);		/* may be the last: exit_mmap, swap_exit */
		if (nskip == ARRAY_SIZE(skip))
			break;
	}
	return left;
}

/* ---- exit, /proc/<pid>/cmdline etc. ----------------------------------------------- */

/* exit_mmap (rp2350_atrans_exit): free the slots of a swapped-out process */
void rp2350_atrans_swap_exit(struct mm_struct *mm)
{
	struct rp2350_atrans_mm *a = mm->context.atrans;
	struct rp2350_atrans_swap *s;
	bool took;

	if (!a || !a->swap)
		return;
	took = swap_get();
	s = a->swap;
	a->swap = NULL;
	if (s) {
		slots_put(s);
		kfree(s);
	}
	swap_put(took);
}

/*
 * access_remote_vm on a swapped-out process (rp2350_atrans_access, mmap
 * read lock held): read straight from swap, so ps and /proc/<pid>/cmdline
 * don't bring processes back. Writes fail. Stops at the first page that is
 * not in swap.
 */
int rp2350_atrans_swap_access(struct mm_struct *mm, unsigned long addr, void *buf, int len,
			      int write)
{
	struct rp2350_atrans_mm *a = mm->context.atrans;
	struct rp2350_atrans_swap *s;
	struct page *page = NULL;
	unsigned long kva, off, pg, slot, n;
	int done = 0;
	unsigned int i;
	bool took;

	if (write || len <= 0 || addr < ATRANS_WINDOW || addr >= ATRANS_WEND)
		return 0;
	took = swap_get();
	kva = mm->context.atrans_kva;
	s = a ? a->swap : NULL;
	if (kva) {
		/* swapped back in meanwhile */
		len = min_t(unsigned long, len, ATRANS_WINDOW + mm->context.atrans_size - addr);
		if (addr >= ATRANS_WINDOW + mm->context.atrans_size)
			len = 0;
		if (len > 0)
			memcpy(buf, loaded == kva ? (void *)addr :
			       notrans(kva) + (addr - ATRANS_WINDOW), len);
		done = len;
		goto out;
	}
	if (!s || s->state != SW_SWAPPED || !sw.bdev)
		goto out;
	page = alloc_page(GFP_KERNEL);
	if (!page)
		goto out;
	st.remote++;
	while (done < len) {
		off = addr + done - ATRANS_WINDOW;
		pg = off >> PAGE_SHIFT;
		for (i = 0; i < s->n; i++)
			if (pg >= s->ext[i].off && pg < s->ext[i].off + s->ext[i].pages)
				break;
		if (i == s->n)
			break;
		slot = s->ext[i].slot + (pg - s->ext[i].off);
		if (swap_rw(REQ_OP_READ, (unsigned long)page_address(page), slot, 1))
			break;
		n = min_t(unsigned long, len - done, PAGE_SIZE - (off & ~PAGE_MASK));
		memcpy(buf + done, page_address(page) + (off & ~PAGE_MASK), n);
		done += n;
	}
	__free_page(page);
out:
	swap_put(took);
	return done;
}

/*
 * For the ATRANS pool's shrinker and rp2350_alloc_exact(): the kernel side
 * is short of memory and the pool has no free space to lend. Swap out the
 * process that slept longest, so that its block's room can be lent (or, for
 * a block outside the pool, its pages are free): an idle session goes to the
 * card instead of the OOM killer taking something. An allocation is waiting
 * (direct reclaim, an FDPIC mmap): any process, the longest asleep first.
 *
 * The swap lock is mostly held across one swap-in or swap-out's card I/O
 * (up to about 400 ms): giving up at once would miss exactly the times
 * memory is tightest (sessions coming back). So wait for it, up to
 * LEND_WAIT_MS, by polling with trylock: if current holds something the holder needs (an
 * mmap lock: with the swap lock held those are only trylocked; the pool
 * mutex: the shrinker takes it only after this), the holder isn't stuck
 * behind us, and the wait just ends. In swap code, or killed: no wait.
 * Reclaim calls the shrinker over and over: after a wait timed out, the
 * next second only trylocks, so one stall stays one LEND_WAIT_MS.
 */
#define LEND_WAIT_MS	400

static unsigned long lend_nowait_until;

bool rp2350_atrans_swap_out_idle(void)
{
	unsigned long until;
	bool ok = false;

	if (!READ_ONCE(sw.bdev) || swap_owner == current)
		return false;
	if (!mutex_trylock(&swap_lock)) {
		if (test_thread_flag(TIF_ATRANS_SWAP) || fatal_signal_pending(current) ||
		    irqs_disabled() || time_before(jiffies, READ_ONCE(lend_nowait_until)))
			return false;
		until = jiffies + msecs_to_jiffies(LEND_WAIT_MS);
		do {
			if (time_after(jiffies, until) || fatal_signal_pending(current)) {
				st.lend_timeouts++;
				WRITE_ONCE(lend_nowait_until, jiffies + HZ);
				return false;
			}
			msleep(10);
		} while (!mutex_trylock(&swap_lock));
		st.lend_waits++;
	}
	set_thread_flag(TIF_ATRANS_SWAP);
	swap_owner = current;
	if (sw.bdev)
		ok = reclaim_one(0);
	swap_put(true);
	if (ok)
		st.lend++;
	return ok;
}

/* ---- background: swap out long sleepers when free memory is low ------------------ */

static void low_work_fn(struct work_struct *w);
static DECLARE_DELAYED_WORK(low_work, low_work_fn);

static void low_work_fn(struct work_struct *w)
{
	/* free pages, and the ATRANS pool's free space (it lends it out) */
	unsigned long free_kb = (global_zone_page_state(NR_FREE_PAGES) << (PAGE_SHIFT - 10)) +
				(atrans_pool_free_bytes() >> 10);
	bool took;

	if (!low_kb)
		return;
	if (free_kb < low_kb && READ_ONCE(sw.bdev)) {
		took = swap_get();
		if (sw.bdev && !draining && reclaim_one((u64)low_age * NSEC_PER_SEC))
			st.background++;
		swap_put(took);
	}
	schedule_delayed_work(&low_work, HZ);
}

/* ---- /proc/atrans-swap: start, stop, statistics ------------------------------------ */

/* A swap file: map its pages to device sectors once (as swapon does), so
 * the I/O bypasses the filesystem. Every page must be 8 contiguous
 * sectors; runs of pages that are also contiguous are merged. */
static int map_file(struct inode *inode, unsigned long pages)
{
	unsigned int bits = inode->i_blkbits, per = PAGE_SIZE >> bits, j;
	unsigned long p, cap = 16;
	struct swrun *r;

	sw.runs = kvmalloc_array(cap, sizeof(*sw.runs), GFP_KERNEL);
	if (!sw.runs)
		return -ENOMEM;
	sw.nruns = 0;
	for (p = 0; p < pages; p++) {
		sector_t b0 = (sector_t)p * per, b, sec;

		if (bmap(inode, &b0) || !b0)
			return -EINVAL;		/* a hole, or no bmap */
		for (j = 1; j < per; j++) {
			b = (sector_t)p * per + j;
			if (bmap(inode, &b) || b != b0 + j)
				return -EINVAL;
		}
		sec = b0 << (bits - SECTOR_SHIFT);
		r = sw.nruns ? &sw.runs[sw.nruns - 1] : NULL;
		if (r && r->page + r->pages == p &&
		    r->sector + ((sector_t)r->pages << (PAGE_SHIFT - SECTOR_SHIFT)) == sec) {
			r->pages++;
			continue;
		}
		if (sw.nruns == cap) {
			struct swrun *n = kvmalloc_array(cap * 2, sizeof(*n), GFP_KERNEL);

			if (!n)
				return -ENOMEM;
			memcpy(n, sw.runs, cap * sizeof(*n));
			kvfree(sw.runs);
			sw.runs = n;
			cap *= 2;
		}
		sw.runs[sw.nruns++] = (struct swrun){ .page = p, .pages = 1, .sector = sec };
	}
	return 0;
}

static void swap_stop(void)
{
	if (sw.file && S_ISREG(file_inode(sw.file)->i_mode)) {
		struct inode *inode = file_inode(sw.file);

		inode_lock(inode);
		inode->i_flags &= ~S_SWAPFILE;
		inode_unlock(inode);
	}
	if (sw.bdev_file)
		fput(sw.bdev_file);
	if (sw.file)
		fput(sw.file);
	kvfree(sw.runs);
	kvfree(sw.map);
	sw.file = sw.bdev_file = NULL;
	WRITE_ONCE(sw.bdev, NULL);
	sw.runs = NULL;
	sw.map = NULL;
	sw.nruns = 0;
	sw.nslots = sw.used = sw.hint = 0;
	sw.name[0] = 0;
}

/* swap_lock held */
static int swap_start(const char *name)
{
	union swap_header *h = NULL;
	struct inode *inode;
	unsigned long pages, last;
	loff_t pos = 0;
	int err;

	sw.file = filp_open(name, O_RDWR | O_LARGEFILE, 0);
	if (IS_ERR(sw.file)) {
		err = PTR_ERR(sw.file);
		sw.file = NULL;
		return err;
	}
	inode = file_inode(sw.file);
	if (S_ISBLK(inode->i_mode)) {
		sw.bdev_file = bdev_file_open_by_dev(inode->i_rdev,
						     BLK_OPEN_READ | BLK_OPEN_WRITE | BLK_OPEN_EXCL,
						     &sw, NULL);
		if (IS_ERR(sw.bdev_file)) {
			err = PTR_ERR(sw.bdev_file);
			sw.bdev_file = NULL;
			goto fail;
		}
		pages = bdev_nr_bytes(file_bdev(sw.bdev_file)) >> PAGE_SHIFT;
	} else if (S_ISREG(inode->i_mode) && inode->i_sb->s_bdev) {
		err = -ETXTBSY;
		if (IS_SWAPFILE(inode))
			goto fail;
		pages = i_size_read(inode) >> PAGE_SHIFT;
	} else {
		err = -EINVAL;
		goto fail;
	}
	err = -ENOMEM;
	h = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!h)
		goto fail;
	err = kernel_read(sw.file, h, PAGE_SIZE, &pos);
	if (err != PAGE_SIZE) {
		err = err < 0 ? err : -EINVAL;
		goto fail;
	}
	err = -EINVAL;
	if (memcmp(h->magic.magic, "SWAPSPACE2", 10) || h->info.version != 1) {
		pr_warn("rp2350-atrans-swap: %s has no swap signature (mkswap it)\n", name);
		goto fail;
	}
	last = min_t(unsigned long, h->info.last_page, pages - 1);
	if (last < 16)
		goto fail;
	if (S_ISREG(inode->i_mode)) {
		err = map_file(inode, last + 1);
		if (err) {
			pr_warn("rp2350-atrans-swap: %s: can't map its blocks (holes, or no bmap)\n", name);
			goto fail;
		}
		sw.bdev = inode->i_sb->s_bdev;
		inode_lock(inode);
		inode->i_flags |= S_SWAPFILE;	/* no writes or truncation meanwhile */
		inode_unlock(inode);
	} else {
		sw.bdev = file_bdev(sw.bdev_file);
	}
	sw.nslots = last;			/* pages 1..last */
	sw.map = kvcalloc(BITS_TO_LONGS(sw.nslots), sizeof(long), GFP_KERNEL);
	err = -ENOMEM;
	if (!sw.map)
		goto fail;
	strscpy(sw.name, name, sizeof(sw.name));
	kfree(h);
	pr_info("rp2350-atrans-swap: %s, %lu KiB%s\n", name, sw.nslots << (PAGE_SHIFT - 10),
		sw.runs ? " (file)" : "");
	return 0;
fail:
	kfree(h);
	swap_stop();
	return err;
}

/*
 * "mkswap PATH": write a Linux swap signature, as mkswap does (Buildroot
 * leaves mkswap out of busybox on nommu): page 0 holds version 1,
 * last_page and "SWAPSPACE2" at its end. Not while it is the swap area.
 */
static int swap_mkswap(const char *name)
{
	union swap_header *h;
	struct file *f;
	struct inode *inode;
	unsigned long pages;
	loff_t pos = 0;
	ssize_t n;
	int err;

	if (sw.bdev && !strcmp(name, sw.name))
		return -EBUSY;
	f = filp_open(name, O_RDWR | O_LARGEFILE, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);
	inode = file_inode(f);
	err = -EINVAL;
	if (S_ISBLK(inode->i_mode))
		pages = bdev_nr_bytes(I_BDEV(f->f_mapping->host)) >> PAGE_SHIFT;
	else if (S_ISREG(inode->i_mode))
		pages = i_size_read(inode) >> PAGE_SHIFT;
	else
		goto out;
	if (pages < 18)
		goto out;
	err = -ENOMEM;
	h = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!h)
		goto out;
	h->info.version = 1;
	h->info.last_page = pages - 1;
	memcpy(h->magic.magic, "SWAPSPACE2", 10);
	n = kernel_write(f, h, PAGE_SIZE, &pos);
	kfree(h);
	err = n == PAGE_SIZE ? vfs_fsync(f, 0) : (n < 0 ? n : -EIO);
	if (!err)
		pr_info("rp2350-atrans-swap: mkswap %s, %lu KiB\n", name,
			(pages - 1) << (PAGE_SHIFT - 10));
out:
	fput(f);
	return err;
}

/* "out PID": swap that process out now, if it is idle (swap_lock held) */
static int swap_out_pid(const char *arg)
{
	struct task_struct *p;
	struct mm_struct *mm = NULL;
	unsigned long pid;
	int err;

	err = kstrtoul(arg, 10, &pid);
	if (err)
		return err;
	rcu_read_lock();
	p = find_task_by_vpid(pid);
	if (p) {
		task_lock(p);
		if (p->mm && p->mm->context.atrans && mmget_not_zero(p->mm))
			mm = p->mm;
		task_unlock(p);
	}
	rcu_read_unlock();
	if (!mm)
		return -ESRCH;
	err = mm == current->mm ? -EBUSY : swap_out(mm);	/* -EBUSY: not idle */
	mmput(mm);
	return err;
}

/*
 * "off": swap every swapped-out process back in, then stop. If some don't
 * fit back in memory, swap stays on and this fails with EBUSY (they are
 * still safe in swap). "off force", or "off" once the device is gone (the
 * card was pulled: its capacity is 0), kills those instead, with a log line
 * each. A process whose data can't be read back is killed either way. None
 * is ever left running on garbage. swap_lock held.
 */
static int swap_off(bool force)
{
	const char *why = NULL;
	int left;

	if (!get_capacity(sw.bdev->bd_disk))
		why = "swapoff with the device gone, and no room for it";	/* card pulled */
	else if (force)
		why = "swapoff force, and no room for it";
	WRITE_ONCE(draining, true);	/* no swap-outs meanwhile */
	left = swap_drain(why);
	WRITE_ONCE(draining, false);
	if (left) {
		pr_warn("rp2350-atrans-swap: swapoff refused: %d swapped-out processes don't fit back in memory (\"off force\" kills them)\n",
			left);
		return -EBUSY;
	}
	if (sw.used)
		pr_warn("rp2350-atrans-swap: swapoff: %lu slots still marked used\n", sw.used);
	pr_info("rp2350-atrans-swap: %s off\n", sw.name);
	swap_stop();
	return 0;
}

static ssize_t swap_write(struct file *f, const char __user *ubuf, size_t len, loff_t *ppos)
{
	char buf[80], *s;
	unsigned long v;
	ssize_t err;
	bool took;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	s = strim(buf);

	if (!strncmp(s, "low ", 4) || !strncmp(s, "age ", 4)) {
		err = kstrtoul(s + 4, 0, &v);
		if (err)
			return err;
		if (s[0] == 'a') {
			low_age = v;
		} else {
			low_kb = v;
			if (v)
				mod_delayed_work(system_wq, &low_work, HZ);
		}
		return len;
	}
	took = swap_get();
	if (!strncmp(s, "out ", 4)) {
		err = sw.bdev && !draining ? swap_out_pid(s + 4) : -EINVAL;
	} else if (!strcmp(s, "off") || !strcmp(s, "off force")) {
		err = sw.bdev ? swap_off(s[3] == ' ') : 0;
	} else if (s[0] == '/') {
		err = sw.bdev ? -EBUSY : swap_start(s);
	} else if (!strncmp(s, "mkswap /", 8)) {
		err = swap_mkswap(s + 7);
	} else {
		err = -EINVAL;
	}
	swap_put(took);
	return err ?: len;
}

static unsigned long kb_per_s(unsigned long pages, u64 ns)
{
	return ns ? div64_u64((u64)pages * (PAGE_SIZE / 1024) * NSEC_PER_SEC, ns) : 0;
}

static int swap_show(struct seq_file *m, void *v)
{
	struct task_struct *p;
	struct mm_struct *mm;
	bool took;

	took = swap_get();
	if (sw.bdev)
		seq_printf(m, "swap %s (%s), %lu KiB, %lu KiB used\n", sw.name,
			   sw.runs ? "file" : "block device", sw.nslots << (PAGE_SHIFT - 10),
			   sw.used << (PAGE_SHIFT - 10));
	else
		seq_puts(m, "swap off\n");
	seq_printf(m, "low %lu KiB (age %u s), free %lu KiB\n", low_kb, low_age,
		   global_zone_page_state(NR_FREE_PAGES) << (PAGE_SHIFT - 10));
	seq_printf(m, "swap-outs %lu, %lu KiB, %llu ms, max %llu ms, %lu KiB/s\n", st.outs,
		   st.out_pages << (PAGE_SHIFT - 10), div_u64(st.out_ns, NSEC_PER_MSEC),
		   div_u64(st.out_max, NSEC_PER_MSEC), kb_per_s(st.out_pages, st.out_ns));
	seq_printf(m, "swap-ins %lu, %lu KiB, %llu ms, max %llu ms, %lu KiB/s\n", st.ins,
		   st.in_pages << (PAGE_SHIFT - 10), div_u64(st.in_ns, NSEC_PER_MSEC),
		   div_u64(st.in_max, NSEC_PER_MSEC), kb_per_s(st.in_pages, st.in_ns));
	seq_printf(m, "targeted %lu, cancelled %lu, background %lu, for the kernel %lu, no victim %lu, no space %lu, io errors %lu, swap-in waits %lu, killed %lu, swapped in by swapoff %lu, swap reads for /proc %lu, window reloads %lu, lock waits for the kernel %lu (%lu timed out), odd layouts %lu, skipped as just cancelled %lu\n",
		   st.targeted, st.cancels, st.background, st.lend, st.nomem, st.nospace, st.ioerr,
		   st.retries, st.killed, st.drained, st.remote, st.reloads, st.lend_waits,
		   st.lend_timeouts, st.odd, st.recancel);
	seq_puts(m, "  pid comm             KiB  block KiB  swapped for\n");
	rcu_read_lock();
	for_each_process(p) {
		struct rp2350_atrans_swap *s;

		task_lock(p);
		mm = p->mm;
		s = mm && mm->context.atrans ? mm->context.atrans->swap : NULL;
		if (s && s->state == SW_SWAPPED)
			seq_printf(m, "%5d %-16s %5lu  %5lu      %lu s\n", task_pid_nr(p), p->comm,
				   s->pages << (PAGE_SHIFT - 10), s->size >> 10,
				   (jiffies - s->when) / HZ);
		task_unlock(p);
	}
	rcu_read_unlock();
	swap_put(took);
	return 0;
}

static int swap_open(struct inode *inode, struct file *file)
{
	return single_open(file, swap_show, NULL);
}

static const struct proc_ops swap_proc_ops = {
	.proc_open = swap_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = swap_write,
};

/* ---- what the rest of the kernel reports ------------------------------------------- */

/*
 * si_swapinfo() (include/linux/swap.h, patched by apply.sh): SwapTotal and
 * SwapFree in /proc/meminfo, sysinfo()'s totalswap and freeswap. Without
 * CONFIG_SWAP there is no other swap, and total_swap_pages stays 0, so
 * reclaim never tries to swap LRU pages. Read without swap_lock, which is
 * held across card I/O: the figures are a moment old at worst.
 */
void rp2350_atrans_si_swapinfo(struct sysinfo *val)
{
	unsigned long n = 0, used = 0;

	if (READ_ONCE(sw.bdev)) {
		n = READ_ONCE(sw.nslots);
		used = min(READ_ONCE(sw.used), n);
	}
	val->totalswap = n;
	val->freeswap = n - used;
}

/* /proc/swaps, as mm/swapfile.c prints it (it needs CONFIG_SWAP) */
static int swaps_show(struct seq_file *m, void *v)
{
	unsigned long kb, used;
	bool took;
	int len;

	seq_puts(m, "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
	took = swap_get();
	if (sw.bdev && sw.file) {
		kb = sw.nslots << (PAGE_SHIFT - 10);
		used = sw.used << (PAGE_SHIFT - 10);
		len = seq_file_path(m, sw.file, " \t\n\\");
		seq_printf(m, "%*s%s\t%lu\t%s%lu\t%s%d\n", len < 40 ? 40 - len : 1, " ",
			   sw.runs ? "file\t" : "partition", kb, kb < 10000000 ? "\t" : "",
			   used, used < 10000000 ? "\t" : "", -2);
	}
	swap_put(took);
	return 0;
}

static int __init rp2350_atrans_swap_init(void)
{
	proc_create("atrans-swap", 0644, NULL, &swap_proc_ops);
	proc_create_single("swaps", 0444, NULL, swaps_show);
	return 0;
}
device_initcall(rp2350_atrans_swap_init);
