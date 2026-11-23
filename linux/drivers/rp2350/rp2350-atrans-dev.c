// SPDX-License-Identifier: GPL-2.0
/*
 * Device mappings for ATRANS processes: MAP_SHARED of a character device
 * that maps directly on nommu (NOMMU_MAP_DIRECT, an address from its
 * get_unmapped_area), such as /dev/pio1 and /dev/pio2 (rp2350-pio.c: the
 * PIO block's registers and the program's DMA buffer).
 *
 * An ATRANS process runs with the MPU confining user mode to its window
 * (rp2350-atrans.c), so a device range must also be opened in the MPU:
 * each process has up to ATRANS_NDEV ranges (struct rp2350_atrans_mm.dev),
 * and while it runs they are MPU regions DEV_REGION0.. (read/write, never
 * executable; Device-nGnRE for MMIO, Normal like the rest of the memory
 * map for RAM). mmap returns the device's address unchanged: user pointers
 * outside the window are only for loads and stores, system calls still
 * refuse them (access_ok).
 *
 * Each range holds a reference to its file, as a VMA would (ATRANS mmaps
 * have no VMAs), so the device is not released (for /dev/pio: the block
 * reset, the DMA buffer freed) while the process can still reach it.
 * munmap, exit and exec drop it; fork gives the child the same ranges (the
 * same device, the file shared as its descriptors are).
 *
 * Hooks in rp2350-atrans.c: mmap, munmap, dup, exit, mpu_confine and
 * show_maps call the functions here.
 */
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/seq_file.h>
#include "rp2350-atrans-internal.h"

#define M33_MPU_TYPE	((void __iomem *)0xe000ed90)
#define M33_MPU_RNR	((void __iomem *)0xe000ed98)
#define M33_MPU_RBAR	((void __iomem *)0xe000ed9c)
#define M33_MPU_RLAR	((void __iomem *)0xe000eda0)
#define M33_MPU_MAIR0	((void __iomem *)0xe000edc0)
#define RBAR_XN		BIT(0)
#define RBAR_AP_RW_ANY	(1 << 1)
#define RLAR_EN		BIT(0)
#define RLAR_ATTR(n)	((n) << 1)
#define ATTR_NORMAL	0		/* MAIR0 attr 0 (rp2350-atrans.c): Normal WT */
#define ATTR_DEVICE	1		/* MAIR0 attr 1 (here): Device-nGnRE */
#define MAIR_DEVICE_NGNRE 0x04

/*
 * MPU regions: rp2350-atrans.c uses 0 (data window), 1 (thread pointer),
 * 2 (text window); 3 and 4 are left for it. Device ranges get 5-7.
 */
#define DEV_REGION0	5

static bool dev_ok;			/* the MPU has the regions */

/* what regions DEV_REGION0.. hold now */
static struct {
	u32 rbar, rlar;
} prog[ATRANS_NDEV];

static bool overlaps(unsigned long s, unsigned long e, unsigned long s2, unsigned long e2)
{
	return s < e2 && s2 < e;
}

/*
 * Program regions DEV_REGION0.. for mm's device ranges (NULL: none).
 * Called from mpu_confine with interrupts off, for whichever process is
 * about to run with the MPU on.
 */
void rp2350_atrans_dev_mpu(struct mm_struct *mm)
{
	struct rp2350_atrans_mm *a = mm ? mm->context.atrans : NULL;
	bool changed = false;
	int i;

	if (!dev_ok)
		return;
	for (i = 0; i < ATRANS_NDEV; i++) {
		struct rp2350_atrans_dev *d = a ? &a->dev[i] : NULL;
		u32 rbar = 0, rlar = 0;

		if (d && d->file) {
			rbar = d->start | RBAR_AP_RW_ANY | RBAR_XN;
			rlar = ((d->end - 1) & ~0x1fUL) | RLAR_EN |
			       RLAR_ATTR(d->start >= 0x40000000 ? ATTR_DEVICE : ATTR_NORMAL);
		}
		if (rbar == prog[i].rbar && rlar == prog[i].rlar)
			continue;
		writel_relaxed(DEV_REGION0 + i, M33_MPU_RNR);
		writel_relaxed(0, M33_MPU_RLAR);	/* off while it changes */
		writel_relaxed(rbar, M33_MPU_RBAR);
		writel_relaxed(rlar, M33_MPU_RLAR);
		prog[i].rbar = rbar;
		prog[i].rlar = rlar;
		changed = true;
	}
	if (changed) {
		dsb(sy);
		isb();
	}
}

/* the current process's regions again, after its ranges changed */
static void reload(struct mm_struct *mm)
{
	unsigned long flags;

	if (mm != current->mm)
		return;
	local_irq_save(flags);
	rp2350_atrans_dev_mpu(mm);
	local_irq_restore(flags);
}

/*
 * The mmap hook (rp2350_atrans_mmap, mmap lock held for writing):
 * -ENOSYS when this is not a device mapping (the caller makes its usual
 * private copy), else the device's address or an error.
 */
unsigned long rp2350_atrans_dev_mmap(struct file *file, unsigned long len, unsigned long prot,
				     unsigned long flags, unsigned long pgoff)
{
	struct mm_struct *mm = current->mm;
	struct rp2350_atrans_mm *a = mm->context.atrans;
	struct rp2350_atrans_dev *free = NULL;
	unsigned long addr, end;
	unsigned int caps;
	int i;

	if (!file || (flags & MAP_TYPE) != MAP_SHARED || !S_ISCHR(file_inode(file)->i_mode) ||
	    !file->f_op->get_unmapped_area || !file->f_op->mmap_capabilities)
		return -ENOSYS;
	caps = file->f_op->mmap_capabilities(file);
	if (!(caps & NOMMU_MAP_DIRECT))
		return -ENOSYS;
	if (!dev_ok)
		return -ENODEV;
	if (flags & MAP_FIXED)
		return -EINVAL;
	if (prot & PROT_EXEC)
		return -EPERM;
	if (((prot & PROT_READ) && !(file->f_mode & FMODE_READ)) ||
	    ((prot & PROT_WRITE) && !(file->f_mode & FMODE_WRITE)))
		return -EACCES;
	if (!len)
		return -EINVAL;
	len = PAGE_ALIGN(len);
	addr = file->f_op->get_unmapped_area(file, 0, len, pgoff, flags);
	if (IS_ERR_VALUE(addr))
		return addr;
	end = addr + len;
	/* 32-byte MPU granules; never over the windows the MPU already
	 * covers (overlapping regions fault) */
	if ((addr & 0x1f) || end <= addr || overlaps(addr, end, ATRANS_WINDOW, ATRANS_WEND) ||
	    overlaps(addr, end, ATRANS_TEXT, ATRANS_TEND))
		return -EINVAL;
	for (i = 0; i < ATRANS_NDEV; i++) {
		struct rp2350_atrans_dev *d = &a->dev[i];

		if (!d->file) {
			if (!free)
				free = d;
			continue;
		}
		if (d->start == addr && d->end == end) {
			d->users++;		/* the same range again */
			return addr;
		}
		if (overlaps(addr, end, d->start, d->end))
			return -EINVAL;
	}
	if (!free)
		return -ENOMEM;
	free->start = addr;
	free->end = end;
	free->users = 1;
	free->file = get_file(file);
	reload(mm);
	return addr;
}

/* The munmap hook: true if [start, start + len) was a device range. */
bool rp2350_atrans_dev_munmap(struct mm_struct *mm, unsigned long start, size_t len)
{
	struct rp2350_atrans_mm *a = mm->context.atrans;
	struct file *f;
	int i;

	if (!a)
		return false;
	for (i = 0; i < ATRANS_NDEV; i++) {
		struct rp2350_atrans_dev *d = &a->dev[i];

		if (!d->file || start != d->start)
			continue;
		if (--d->users)
			return true;
		f = d->file;
		d->file = NULL;
		reload(mm);		/* closed before the file can go */
		fput(f);
		return true;
	}
	return false;
}

/* fork: the child's copy of the table (kmemdup'd) takes its references */
void rp2350_atrans_dev_dup(struct rp2350_atrans_mm *a)
{
	int i;

	for (i = 0; i < ATRANS_NDEV; i++)
		if (a->dev[i].file)
			get_file(a->dev[i].file);
}

/* exit and exec: the process no longer runs (or never again with these) */
void rp2350_atrans_dev_exit(struct rp2350_atrans_mm *a)
{
	int i;

	for (i = 0; i < ATRANS_NDEV; i++) {
		if (a->dev[i].file) {
			fput(a->dev[i].file);
			a->dev[i].file = NULL;
		}
	}
}

void rp2350_atrans_dev_show(struct seq_file *m, struct mm_struct *mm)
{
	struct rp2350_atrans_mm *a = mm->context.atrans;
	int i;

	for (i = 0; a && i < ATRANS_NDEV; i++) {
		struct rp2350_atrans_dev *d = &a->dev[i];

		if (!d->file)
			continue;
		seq_printf(m, "%08lx-%08lx rw-s 00000000 00:00 0", d->start, d->end);
		seq_pad(m, ' ');
		seq_file_path(m, d->file, "\n");
		seq_putc(m, '\n');
	}
}

/* after rp2350_atrans_init (core_initcall), which writes MAIR0 attr 0 */
static int __init rp2350_atrans_dev_init(void)
{
	unsigned long flags;

	if ((readl_relaxed(M33_MPU_TYPE) >> 8 & 0xff) < DEV_REGION0 + ATRANS_NDEV) {
		pr_warn("rp2350-atrans: too few MPU regions for device mappings\n");
		return 0;
	}
	local_irq_save(flags);
	writel_relaxed((readl_relaxed(M33_MPU_MAIR0) & ~0xff00) | MAIR_DEVICE_NGNRE << 8,
		       M33_MPU_MAIR0);
	dsb(sy);
	isb();
	local_irq_restore(flags);
	dev_ok = true;
	return 0;
}
postcore_initcall(rp2350_atrans_dev_init);
