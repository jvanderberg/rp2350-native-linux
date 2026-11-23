// SPDX-License-Identifier: GPL-2.0
/*
 * CONFIG_RP2350_MEMSTAT: where the kernel's memory goes, for the 8 MiB
 * board. Included at the end of mm/slub.c (apply.sh), which has the SLUB
 * internals it reads. Without SLUB_DEBUG (SLUB_TINY) there is no
 * /proc/slabinfo, and allocations over KMALLOC_MAX_CACHE_SIZE (8 KiB) go
 * straight to the page allocator while still counting as SUnreclaim.
 *
 * /proc/memstat:
 *   every slab cache with pages: object size, slab order, pages, objects
 *   in use, partial slabs (kept empty ones included), bytes wasted
 *   the live large kmallocs, grouped by caller (vmalloc on nommu is kmalloc,
 *   rounded up to a power of two)
 *   all pages by kind (free, slab, large kmalloc, LRU, reserved, other)
 * /proc/memmap: one character per page, 64 pages (256 KiB) a line:
 *   . free  S slab  L large kmalloc  C LRU/page cache  R reserved
 *   o other (MmapCopy, kernel stacks, ATRANS blocks, pipes, frags, ...)
 *   0 count 0: on a per-cpu free list, or a later page of a non-compound
 *     higher-order allocation (kernel stacks)
 */
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mmzone.h>

#define RPMS_LARGE 192
static struct {
	void *p;
	unsigned long size, caller;
} rpms_large[RPMS_LARGE];
static unsigned long rpms_large_lost;
static DEFINE_SPINLOCK(rpms_lock);

void rp2350_memstat_large(void *p, size_t size, unsigned long caller)
{
	unsigned long flags;
	int i, fr = -1;

	if (!p)
		return;
	spin_lock_irqsave(&rpms_lock, flags);
	for (i = 0; i < RPMS_LARGE; i++) {
		if (rpms_large[i].p == p) {	/* __do_kmalloc_node: the better caller */
			rpms_large[i].caller = caller;
			goto out;
		}
		if (!rpms_large[i].p && fr < 0)
			fr = i;
	}
	if (fr < 0) {
		rpms_large_lost++;
		goto out;
	}
	rpms_large[fr].p = p;
	rpms_large[fr].size = size;
	rpms_large[fr].caller = caller;
out:
	spin_unlock_irqrestore(&rpms_lock, flags);
}

void rp2350_memstat_large_free(void *p)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&rpms_lock, flags);
	for (i = 0; i < RPMS_LARGE; i++)
		if (rpms_large[i].p == p) {
			rpms_large[i].p = NULL;
			break;
		}
	spin_unlock_irqrestore(&rpms_lock, flags);
}

static bool rpms_is_large(unsigned long pfn)
{
	int i;

	for (i = 0; i < RPMS_LARGE; i++) {
		unsigned long s;

		if (!rpms_large[i].p)
			continue;
		s = virt_to_pfn(rpms_large[i].p);
		if (pfn >= s && pfn < s + (1UL << get_order(rpms_large[i].size)))
			return true;
	}
	return false;
}

static char rpms_kind(struct zone *z, unsigned long pfn, unsigned long *skip)
{
	struct page *pg = pfn_to_page(pfn);

	*skip = 1;
	if (PageBuddy(pg)) {
		*skip = 1UL << buddy_order(pg);
		return '.';
	}
	if (PageSlab(pg))
		return 'S';
	if (PageReserved(pg))
		return 'R';
	if (PageLRU(pg) || (page_count(pg) && folio_mapping(page_folio(pg))))
		return 'C';
	if (rpms_is_large(pfn))
		return 'L';
	if (!page_count(pg) && !PageTail(pg))
		return '0';	/* on a per-cpu free list, or the tail of a non-compound block */
	return 'o';
}

static int memstat_show(struct seq_file *m, void *v)
{
	unsigned long tot_pages = 0, tot_used = 0, flags;
	unsigned long kinds[128] = { 0 };
	struct kmem_cache *s;
	struct zone *z;
	int i, j;

	seq_puts(m, "cache                    objsz ord obj/slab pages  inuse partial(empty)  usedKiB wasteKiB\n");
	mutex_lock(&slab_mutex);
	list_for_each_entry(s, &slab_caches, list) {
		struct kmem_cache_node *n = get_node(s, 0);
		unsigned long pages = atomic_long_read(&s->rp_pages);
		unsigned long objs = oo_objects(s->oo), order = oo_order(s->oo);
		unsigned long slabs, partial = 0, empty = 0, pinuse = 0, inuse;
		struct slab *sl;

		if (!pages || !n)
			continue;
		spin_lock_irqsave(&n->list_lock, flags);
		list_for_each_entry(sl, &n->partial, slab_list) {
			partial++;
			pinuse += sl->inuse;
			if (!sl->inuse)
				empty++;
		}
		spin_unlock_irqrestore(&n->list_lock, flags);
		/* slabs of other orders (s->min) are rare: count by pages */
		slabs = pages >> order;
		inuse = (slabs - partial) * objs + pinuse;
		seq_printf(m, "%-24s %5u %3lu %8lu %5lu %6lu %7lu(%lu) %8lu %8lu\n", s->name,
			   s->object_size, order, objs, pages, inuse, partial, empty,
			   inuse * s->object_size >> 10,
			   ((pages << PAGE_SHIFT) - inuse * s->object_size) >> 10);
		tot_pages += pages;
		tot_used += inuse * s->object_size;
	}
	mutex_unlock(&slab_mutex);
	seq_printf(m, "slab total %lu KiB, objects %lu KiB\n\n", tot_pages << (PAGE_SHIFT - 10),
		   tot_used >> 10);

	seq_printf(m, "large kmalloc (lost %lu):\n", rpms_large_lost);
	spin_lock_irqsave(&rpms_lock, flags);
	for (i = 0; i < RPMS_LARGE; i++) {
		unsigned long n = 0, bytes = 0, req = 0;

		if (!rpms_large[i].p)
			continue;
		for (j = 0; j < i; j++)
			if (rpms_large[j].p && rpms_large[j].caller == rpms_large[i].caller)
				break;
		if (j < i)
			continue;	/* counted with the first */
		for (j = i; j < RPMS_LARGE; j++)
			if (rpms_large[j].p && rpms_large[j].caller == rpms_large[i].caller) {
				n++;
				req += rpms_large[j].size;
				bytes += PAGE_SIZE << get_order(rpms_large[j].size);
			}
		seq_printf(m, "  %3lu x %7lu KiB (asked %7lu)  %pS\n", n, bytes >> 10, req >> 10,
			   (void *)rpms_large[i].caller);
	}
	spin_unlock_irqrestore(&rpms_lock, flags);

	for_each_populated_zone(z) {
		unsigned long pfn, skip;

		for (pfn = z->zone_start_pfn; pfn < zone_end_pfn(z); pfn += skip)
			kinds[(int)rpms_kind(z, pfn, &skip)] += skip;
	}
	seq_printf(m, "\npages (KiB): free %lu slab %lu large %lu lru/cache %lu reserved %lu other %lu count0 %lu\n",
		   kinds['.'] * 4, kinds['S'] * 4, kinds['L'] * 4, kinds['C'] * 4, kinds['R'] * 4,
		   kinds['o'] * 4, kinds['0'] * 4);
	return 0;
}

static int memmap_show(struct seq_file *m, void *v)
{
	struct zone *z;

	for_each_populated_zone(z) {
		unsigned long pfn, skip, k, col = 0;

		for (pfn = z->zone_start_pfn; pfn < zone_end_pfn(z); pfn += skip) {
			char c = rpms_kind(z, pfn, &skip);

			for (k = 0; k < skip; k++) {
				if (!col)
					seq_printf(m, "%08lx ", (unsigned long)page_address(pfn_to_page(pfn + k)));
				seq_putc(m, c);
				if (++col == 64) {
					seq_putc(m, '\n');
					col = 0;
				}
			}
		}
	}
	return 0;
}

static int __init rp2350_memstat_init(void)
{
	proc_create_single("memstat", 0444, NULL, memstat_show);
	proc_create_single("memmap", 0444, NULL, memmap_show);
	return 0;
}
late_initcall(rp2350_memstat_init);
