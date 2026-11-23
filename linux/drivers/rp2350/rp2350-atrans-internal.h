/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Internals of rp2350-atrans.c shared with rp2350-atrans-swap.c. Not an ABI:
 * interrupts off for the window/MPU/cache helpers, mmap lock where noted.
 */
#ifndef RP2350_ATRANS_INTERNAL_H
#define RP2350_ATRANS_INTERNAL_H

#include <linux/list.h>
#include <linux/mm_types.h>
#include <linux/time64.h>

#define ATRANS_WINDOW	0x11000000UL
#define ATRANS_MAX	(4 * 1024 * 1024UL)
#define ATRANS_WEND	(ATRANS_WINDOW + ATRANS_MAX)
#define ATRANS_PAGES	(ATRANS_MAX >> PAGE_SHIFT)
#define ATRANS_HEAP0	(64 * 1024UL)	/* heap room at exec */
#define ATRANS_SLACK	(32 * 1024UL)	/* extra room on growth and in a fork child */
#define ATRANS_SHRINK	(64 * 1024UL)	/* shrink only when this much (and 1/4) is free */
#define ATRANS_MINSTACK	(8 * 1024UL)	/* smallest stack region accepted */
#define ATRANS_TP	ATRANS_WINDOW	/* thread pointer, read-only for user mode */
#define ATRANS_TPSIZE	32		/* one MPU granule */

#define ATRANS_TEXT	0x11C00000UL	/* shared text window (ATRANS7) */
#define ATRANS_TMAX	(4 * 1024 * 1024UL)
#define ATRANS_TEND	(ATRANS_TEXT + ATRANS_TMAX)

#define QMI_ATRANS4	((void __iomem *)0x400d0044)
#define QMI_ATRANS7	((void __iomem *)0x400d0050)
#define XIP_MAINT	0x18000000UL
#define XIP_SETWAY_HI	0x03ffc000UL	/* docs/qmi-atrans.md: set/way clean rewrites the tag */
#define XIP_CACHE	(16 * 1024)
#define XIP_LINE	8
#define PSRAM_NOTRANS	0x1d000000UL	/* uncached, untranslated alias */

#define DWT_CYCCNT	((void __iomem *)0xe0001004)

/*
 * Shared text (ATRANS v3): the read-only PT_LOADs a program links in the
 * text window, loaded once per file into one block that every process
 * running the file maps through ATRANS7. Keyed by inode (plus size and
 * mtime; exec denies writes to a running binary anyway), counted, freed
 * with its last user.
 */
struct atrans_text {
	struct list_head list;
	struct inode *inode;		/* held */
	loff_t isize;
	struct timespec64 mtime;
	unsigned long kva, size;	/* block: window offset 0 = ATRANS_TEXT */
	unsigned long start, end;	/* lowest text p_vaddr, highest end */
	int users;			/* processes (mms), under text_lock */
};

/* a device range an ATRANS process mapped (rp2350-atrans-dev.c) */
#define ATRANS_NDEV	3
struct rp2350_atrans_dev {
	unsigned long start, end;	/* the device's addresses */
	struct file *file;		/* held; NULL: free slot */
	int users;			/* mmaps of this range */
};

/* per-process state beyond mm->context.atrans_kva/atrans_size */
struct rp2350_atrans_mm {
	unsigned long stack_top;			/* lowest data-window p_vaddr */
	struct atrans_text *text;			/* NULL: v2 single window */
	struct rp2350_atrans_swap *swap;		/* rp2350-atrans-swap.c */
	unsigned long map[BITS_TO_LONGS(ATRANS_PAGES)];	/* mmapped pages */
	struct rp2350_atrans_dev dev[ATRANS_NDEV];	/* device mappings */
};

/* rp2350-atrans-dev.c: device mappings (MPU regions 5-7) */
struct seq_file;
void rp2350_atrans_dev_mpu(struct mm_struct *mm);	/* interrupts off */
unsigned long rp2350_atrans_dev_mmap(struct file *file, unsigned long len, unsigned long prot,
				     unsigned long flags, unsigned long pgoff); /* -ENOSYS: not one */
bool rp2350_atrans_dev_munmap(struct mm_struct *mm, unsigned long start, size_t len);
void rp2350_atrans_dev_dup(struct rp2350_atrans_mm *a);
void rp2350_atrans_dev_exit(struct rp2350_atrans_mm *a);
void rp2350_atrans_dev_show(struct seq_file *m, struct mm_struct *mm);

struct rp2350_atrans_swap;

extern unsigned long loaded;		/* kva of the block ATRANS4 maps, 0: none */

static inline void *notrans(unsigned long kva)	/* uncached, untranslated alias */
{
	return (void *)(PSRAM_NOTRANS + kva - CONFIG_DRAM_BASE);
}

/* cache and windows (interrupts off) */
void xip_flush_all(void);			/* clean + invalidate everything */
void atrans_map(unsigned long kva, unsigned long size);	/* ATRANS4 (after a flush) */
void text_map(struct mm_struct *mm);		/* ATRANS7 (after a flush) */
void mpu_confine(struct mm_struct *mm);		/* MPU regions for mm, NULL: off */
void tp_store(unsigned long tp);		/* the thread pointer word */
/* mm (current) into the windows: flush, ATRANS4/7, MPU, thread pointer.
 * Takes care of interrupts itself; a no-op if its block is loaded. */
void ensure_loaded(struct mm_struct *mm);

/* blocks */
unsigned long block_alloc(unsigned long size);	/* may sleep; 0: ENOMEM */
void block_free(unsigned long kva, unsigned long size);
void set_block(struct mm_struct *mm, unsigned long kva, unsigned long size);
unsigned long used_top(struct mm_struct *mm);	/* brk or the highest mapped page */
void atrans_for_each_used(struct mm_struct *mm, unsigned long sp,
			  void (*fn)(unsigned long off, unsigned long len, void *arg), void *arg);

/* the ATRANS pool (rp2350-atrans-pool.c), under block_alloc()/block_free() */
unsigned long block_alloc_run(unsigned long size);	/* page allocator run, 0: none */
unsigned long atrans_run_take_at(unsigned long kva, unsigned long max);	/* bytes taken */
unsigned long atrans_pool_lend(unsigned long pages);	/* pages given to the page allocator */
unsigned long atrans_pool_free_bytes(void);		/* free space in the pool */
bool atrans_may_move(struct mm_struct *mm, struct atrans_text *t);
bool atrans_block_move(struct mm_struct *mm, struct atrans_text *t, unsigned long src,
		       unsigned long dst, unsigned long size);	/* false: can't now */
bool atrans_pool_has(unsigned long kva);
unsigned long atrans_pool_alloc(unsigned long size);	/* 0: no room */
void atrans_pool_free(unsigned long kva);
bool atrans_pool_extend(unsigned long kva, unsigned long nsize);
void atrans_pool_trim(unsigned long kva, unsigned long nsize);
void atrans_pool_own(unsigned long kva, struct mm_struct *mm, struct atrans_text *t);
void atrans_pool_mark_free(u8 *cls, unsigned long start_pfn, unsigned long end_pfn);
unsigned long atrans_pool_pick(unsigned long pages, const unsigned long *kva,
			       const unsigned long *npages, unsigned int n, bool *pick);
void atrans_pool_show(struct seq_file *m);
void atrans_pool_diag(unsigned long *size, unsigned long *free, unsigned long *largest);
int rp2350_atrans_swap_diag(char *buf, int len);	/* the last swap_reclaim's reasons */
void atrans_pool_init(void);

/* implemented by rp2350-atrans-swap.c (weak defaults in rp2350-atrans.c) */
bool rp2350_atrans_swap_reclaim(unsigned long size);	/* true: retry, memory freed */
void rp2350_atrans_swap_exit(struct mm_struct *mm);	/* free a->swap */
bool rp2350_atrans_swap_out_idle(void);		/* true: one process swapped out */
int rp2350_atrans_swap_access(struct mm_struct *mm, unsigned long addr, void *buf,
			      int len, int write);	/* /proc access, swapped out */

#endif
