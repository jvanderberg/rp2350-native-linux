// SPDX-License-Identifier: GPL-2.0
/*
 * The ATRANS pool: a physically contiguous piece of PSRAM taken at boot
 * (atrans_pool= on the command line, default 5 MiB, 0: none) from which
 * rp2350-atrans.c's block_alloc() takes ATRANS blocks (data and shared
 * text) first. Kernel and FDPIC allocations never land in it, so every
 * page of the pool is either free or part of an ATRANS block.
 *
 * And ATRANS blocks can move: a process sees only its windows, so a block
 * that isn't in use can be copied elsewhere and the window repointed at the
 * next switch (atrans_block_move() in rp2350-atrans.c). When no gap in the
 * pool fits a request but the pool has enough free space, the pool is
 * compacted: in the stretch between unmovable blocks that needs the least
 * copying, the movable blocks slide down to close the gaps, which leaves
 * one free run at its top. Unmovable: the current process's block and
 * text (the caller may be using them), except while it grows or forks
 * (then its windows are repointed at once), a block whose mmap lock is
 * held (growth, fork, swap-out, /proc readers), and a block not yet owned
 * (being filled: exec, fork, swap-in). Nothing here sleeps while blocks move, and
 * the kernel is uniprocessor and not preemptible, so no task runs in a
 * block while it moves.
 *
 * The pool is elastic. Its end moves: free space at its top is lent to the
 * page allocator when the kernel side runs short (a shrinker, and
 * rp2350_alloc_exact() for FDPIC memory), and the pool grows back into the
 * free pages past its end when an ATRANS block needs the room. It never
 * shrinks below atrans_pool_min= (default 2 MiB). Kernel memory is not
 * movable and FDPIC programs are not swappable, so the kernel side comes
 * first: ATRANS blocks can be swapped out, a failed kernel allocation runs
 * the OOM killer.
 *
 * When the pool can't satisfy a request, block_alloc() falls back to the
 * page allocator (then cache drops, then swap).
 *
 * Bookkeeping: one extent per block, in a list sorted by address, under
 * pool_mutex. Free space is what lies between extents.
 */
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/memblock.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/shrinker.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/swap.h>
#include <linux/vmstat.h>
#include <linux/workqueue.h>
#include <linux/mmzone.h>
#include <asm/rp2350-atrans.h>
#include "rp2350-atrans-internal.h"

struct pool_ext {
	struct list_head list;
	unsigned long kva, size;
	struct mm_struct *mm;		/* a process's data block, or */
	struct atrans_text *text;	/* a shared text block; neither: being filled */
};

static unsigned long pool_base, pool_end;	/* kva; pool_end == pool_base: no pool */
static unsigned long pool_hi;			/* the furthest pool_end may grow (end of RAM) */
static unsigned long pool_used;			/* bytes in extents */
static unsigned long pool_req = 5UL << 20, pool_min = 2UL << 20;
static LIST_HEAD(exts);
static DEFINE_MUTEX(pool_mutex);
static struct {
	unsigned long allocs, fails, compactions, moves, moved_bytes, move_us_max;
	unsigned long lent, lends, grown, grows;	/* bytes and times */
	unsigned long lent_scan;			/* bytes, by the shrinker */
} pst;

static int __init atrans_pool_param(char *s)
{
	pool_req = PAGE_ALIGN(memparse(s, NULL));
	return 0;
}
early_param("atrans_pool", atrans_pool_param);

static int __init atrans_pool_min_param(char *s)
{
	pool_min = PAGE_ALIGN(memparse(s, NULL));
	return 0;
}
early_param("atrans_pool_min", atrans_pool_min_param);

bool atrans_pool_has(unsigned long kva)
{
	return kva >= pool_base && kva < pool_end;
}

static struct pool_ext *ext_find(unsigned long kva)
{
	struct pool_ext *e;

	list_for_each_entry(e, &exts, list)
		if (e->kva == kva)
			return e;
	return NULL;
}

/* end of the free space after e (NULL: from the pool's start) */
static unsigned long gap_end(struct pool_ext *e)
{
	struct list_head *n = e ? e->list.next : exts.next;

	return n == &exts ? pool_end : list_entry(n, struct pool_ext, list)->kva;
}

/* best fit: the smallest gap that holds size; 0 if none. *after: the
 * extent the gap follows (NULL: the pool's start). */
static unsigned long fit(unsigned long size, struct pool_ext **after)
{
	unsigned long best = 0, bestlen = ULONG_MAX, s, len;
	struct pool_ext *e, *prev = NULL;

	s = pool_base;
	list_for_each_entry(e, &exts, list) {
		len = e->kva - s;
		if (len >= size && len < bestlen) {
			best = s;
			bestlen = len;
			*after = prev;
		}
		s = e->kva + e->size;
		prev = e;
	}
	len = pool_end - s;
	if (len >= size && len < bestlen) {
		best = s;
		*after = prev;
	}
	return best;
}

static bool movable(struct pool_ext *e)
{
	return atrans_may_move(e->mm, e->text);
}

static bool move_ext(struct pool_ext *e, unsigned long dst)
{
	if (!atrans_block_move(e->mm, e->text, e->kva, dst, e->size))
		return false;
	pst.moves++;
	pst.moved_bytes += e->size;
	e->kva = dst;
	return true;
}

static void time_moves(u64 t0)
{
	t0 = div_u64(ktime_get_ns() - t0, 1000);
	pst.move_us_max = max_t(unsigned long, pst.move_us_max, t0);
}

/* Slide the extents from `first` that start below `end` down to dst, closing
 * the gaps between them. One that can't move stays, and the slide goes on
 * after it. */
static void slide_down(struct pool_ext *first, unsigned long dst, unsigned long end)
{
	struct pool_ext *e;
	u64 t0 = ktime_get_ns();

	pst.compactions++;
	for (e = first; !list_entry_is_head(e, &exts, list) && e->kva < end;
	     e = list_next_entry(e, list)) {
		if (e->kva != dst && !move_ext(e, dst))
			dst = e->kva;
		dst += e->size;
	}
	time_moves(t0);
}

static void compact_top(void);

/* where the free space at the pool's top starts */
static unsigned long top_start(void)
{
	struct pool_ext *e;

	if (list_empty(&exts))
		return pool_base;
	e = list_last_entry(&exts, struct pool_ext, list);
	return e->kva + e->size;
}

/* Grow the pool's end by up to `want` bytes into free pages past it. */
static unsigned long pool_grow(unsigned long want)
{
	unsigned long got;

	want = min(PAGE_ALIGN(want), pool_hi - pool_end);
	got = atrans_run_take_at(pool_end, want);
	if (got) {
		pool_end += got;
		pst.grown += got;
		pst.grows++;
	}
	return got;
}

/* Lend up to `pages` pages of free space at the pool's top to the page
 * allocator, compacting first if the top lacks them; pool_mutex held. */
static unsigned long pool_lend_locked(unsigned long pages)
{
	unsigned long want = pages << PAGE_SHIFT, top, rel, flags;

	if (!want || pool_end - pool_base <= pool_min)
		return 0;
	want = min(want, pool_end - pool_base - pool_min);
	if (pool_end - top_start() < want && pool_end - pool_base - pool_used > pool_end - top_start())
		compact_top();
	top = max(top_start(), pool_base + pool_min);
	if (top >= pool_end)
		return 0;
	rel = min(want, pool_end - top);
	local_irq_save(flags);
	xip_flush_all();		/* no line of the pool's may land in them later */
	local_irq_restore(flags);
	pool_end -= rel;
	free_pages_exact((void *)pool_end, rel);
	pst.lent += rel;
	pst.lends++;
	return rel >> PAGE_SHIFT;
}

/* Read without pool_mutex (/proc/meminfo, sysinfo): every update keeps
 * pool_used within the pool, and a stale value is only a moment old. */
unsigned long atrans_pool_free_bytes(void)
{
	unsigned long size = READ_ONCE(pool_end) - pool_base, used = READ_ONCE(pool_used);

	return size > used ? size - used : 0;
}

unsigned long atrans_pool_size(void)
{
	return READ_ONCE(pool_end) - pool_base;
}

/* rp2350_alloc_exact(): FDPIC memory found no run */
unsigned long atrans_pool_lend(unsigned long pages)
{
	unsigned long n;

	if (pool_end == pool_base)
		return 0;
	mutex_lock(&pool_mutex);
	n = pool_lend_locked(pages);
	mutex_unlock(&pool_mutex);
	return n;
}

/*
 * The shrinker: when an allocation is waiting (direct reclaim), lend the
 * pool's free space, but only once the page cache is mostly gone. Space
 * lent comes back only if nothing unmovable lands next to it, so the cache
 * goes first, and kswapd (keeping its watermarks, nobody waiting) lends
 * nothing: lending idle sessions' room to it would leave the pool at its
 * minimum and the free pages scattered, where no ATRANS block fits.
 */
#define POOL_CACHE_LOW	(256UL << 10)

/* reclaimable page cache: the file LRUs. Not NR_FILE_PAGES, which counts
 * ramfs (tmpfs here) pages too: they sit on the unevictable list, so a few
 * hundred KiB in /tmp and /run would keep the cache "not low", and nothing
 * would be lent with free pages at the min watermark and megabytes free in
 * the pool. */
static unsigned long file_lru_pages(void)
{
	return global_node_page_state(NR_ACTIVE_FILE) +
	       global_node_page_state(NR_INACTIVE_FILE);
}

static unsigned long pool_lendable(void)
{
	unsigned long file, freeb;

	if (pool_end - pool_base <= pool_min)
		return 0;
	file = file_lru_pages();
	if ((file << PAGE_SHIFT) > POOL_CACHE_LOW)
		return 0;
	freeb = pool_end - pool_base - pool_used;
	return min(freeb, pool_end - pool_base - pool_min) >> PAGE_SHIFT;
}

/* free pages below the high watermark: the kernel side is really short,
 * not just missing a run (lending for a missing run goes on in batches
 * until the pool hits its minimum, megabytes at boot) */
static bool pages_short(void)
{
	unsigned long high = 0;
	struct zone *z;

	for_each_populated_zone(z)
		high += high_wmark_pages(z);
	return global_zone_page_state(NR_FREE_PAGES) < high;
}

/* page cache mostly gone: the kernel side's need is real */
static bool cache_low(void)
{
	return (file_lru_pages() << PAGE_SHIFT) <= POOL_CACHE_LOW;
}

static unsigned long pool_count(struct shrinker *sh, struct shrink_control *sc)
{
	unsigned long n;

	if ((current->flags & PF_KSWAPD) || !pages_short())
		return 0;
	n = pool_lendable();

	/* nothing free to lend: swapping an idle process out makes some (or
	 * frees its pages, for a block outside the pool) */
	if (!n && cache_low())
		n = 32;
	return n;
}

/* the shrinker's lending: the pool may be busy (a move, a growth) */
static unsigned long pool_scan_lend(unsigned long nr)
{
	unsigned long n = 0;

	nr = min(nr, pool_lendable());
	if (nr && mutex_trylock(&pool_mutex)) {
		n = pool_lend_locked(nr);
		pst.lent_scan += n << PAGE_SHIFT;
		mutex_unlock(&pool_mutex);
	}
	return n;
}

static unsigned long pool_scan(struct shrinker *sh, struct shrink_control *sc)
{
	unsigned long n, freed = 0, f0;
	long d;

	/* not for an allocation that may fail (opportunistic, or one with a
	 * fallback of its own: block_alloc, rp2350_alloc_exact) */
	if (sc->gfp_mask & __GFP_NORETRY)
		return SHRINK_STOP;
	if ((current->flags & PF_KSWAPD) || !pages_short())
		return SHRINK_STOP;
	n = pool_scan_lend(sc->nr_to_scan);
	/* nothing lent: no free space, or what there is can't be gathered at
	 * the top (a block that can't move: its mmap lock is taken), or the
	 * pool is busy. Swap out the process asleep longest (the card: only
	 * where I/O may be started), then lend what that freed. Counting the
	 * pool's free space as reclaimed without lending it would leave a
	 * GFP_KERNEL allocation to the OOM killer with hundreds of KiB free in
	 * the pool behind one pinned block. */
	if (!n && cache_low() && (sc->gfp_mask & __GFP_IO) && (sc->gfp_mask & __GFP_FS)) {
		f0 = global_zone_page_state(NR_FREE_PAGES);
		if (rp2350_atrans_swap_out_idle()) {
			d = global_zone_page_state(NR_FREE_PAGES) - f0;
			freed = d > 0 ? d : 0;	/* a block outside the pool */
			n = pool_scan_lend(sc->nr_to_scan);
		}
	}
	n += freed;
	if (!n)
		return SHRINK_STOP;
	mm_account_reclaimed_pages(n);
	return n;
}

/*
 * Make a free run of size bytes. Stretches are the spans between unmovable
 * extents; sliding a stretch's movable extents down leaves all its free
 * space in one run at its top. Pick the stretch with room that needs the
 * fewest bytes copied, and slide it. Returns whether it made the run (a
 * block can still refuse to move: its mmap lock is taken).
 */
static bool compact(unsigned long size)
{
	struct pool_ext *e, *first, *best_first = NULL;
	unsigned long s, used, cost, best_cost = ULONG_MAX, dst, end, best_end = 0;

	/* scan stretches: [s, end), extents from `first` up to the next unmovable one */
	s = pool_base;
	first = list_first_entry(&exts, struct pool_ext, list);	/* the head if empty */
	e = first;
	used = cost = 0;
	dst = s;
	for (;;) {
		bool at_end = list_entry_is_head(e, &exts, list);

		if (at_end || !movable(e)) {
			end = at_end ? pool_end : e->kva;
			if (end - s - used >= size && cost < best_cost) {
				best_cost = cost;
				best_first = first;
				best_end = end;
				dst = s;
			}
			if (at_end)
				break;
			s = e->kva + e->size;
			first = list_next_entry(e, list);
			used = cost = 0;
			e = first;
			continue;
		}
		/* where it lands when its stretch slides down: copied if it moves */
		if (e->kva != s + used)
			cost += e->size;
		used += e->size;
		e = list_next_entry(e, list);
	}
	if (!best_cost || best_cost == ULONG_MAX)
		return false;		/* no stretch can do it, or nothing to move */
	slide_down(best_first, dst, best_end);
	return true;
}

/* Slide the movable extents of the last stretch (after the last unmovable
 * extent) down: all its free space ends up at the pool's top. */
static void compact_top(void)
{
	struct pool_ext *e, *first = list_first_entry(&exts, struct pool_ext, list);
	unsigned long dst = pool_base;

	list_for_each_entry(e, &exts, list)
		if (!movable(e)) {
			dst = e->kva + e->size;
			first = list_next_entry(e, list);
		}
	if (!list_entry_is_head(first, &exts, list))
		slide_down(first, dst, pool_end);
}

/* A block of size bytes from the pool, owned by nobody yet; 0: none. */
unsigned long atrans_pool_alloc(unsigned long size)
{
	struct pool_ext *e, *after = NULL;
	unsigned long kva;

	if (pool_end == pool_base || size > pool_end - pool_base)
		return 0;
	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return 0;
	mutex_lock(&pool_mutex);
	kva = fit(size, &after);
	/* the pool's top plus free pages past it, then compaction, then that again */
	if (!kva && pool_grow(size - min(size, pool_end - top_start())))
		kva = fit(size, &after);
	if (!kva && compact(size))
		kva = fit(size, &after);
	if (!kva && pool_grow(size - min(size, pool_end - top_start())))
		kva = fit(size, &after);
	if (!kva) {
		pst.fails++;
		mutex_unlock(&pool_mutex);
		kfree(e);
		return 0;
	}
	e->kva = kva;
	e->size = size;
	list_add(&e->list, after ? &after->list : &exts);
	pool_used += size;
	pst.allocs++;
	mutex_unlock(&pool_mutex);
	return kva;
}

void atrans_pool_free(unsigned long kva)
{
	struct pool_ext *e;

	mutex_lock(&pool_mutex);
	e = ext_find(kva);
	if (e) {
		list_del(&e->list);
		pool_used -= e->size;
	}
	mutex_unlock(&pool_mutex);
	WARN_ON_ONCE(!e);
	kfree(e);
}

/*
 * Make room for e to grow to nsize in place. In e's stretch (between the
 * unmovable extents around it, or the pool's ends), slide the extents
 * before it, and e itself when it may move (a growing process allows it),
 * down, and those after it up: all the stretch's free space ends up right
 * after e. A process that grows keeps its block, so it needs no second one
 * to copy into: only the growth has to be free.
 */
static void make_room(struct pool_ext *e, unsigned long nsize)
{
	struct pool_ext *x, *first = e, *last = e;
	unsigned long lo = e->kva, hi = pool_end, used = 0, dst;
	u64 t0;

	if (movable(e)) {
		lo = pool_base;
		for (x = list_prev_entry(e, list); !list_entry_is_head(x, &exts, list);
		     x = list_prev_entry(x, list)) {
			if (!movable(x)) {
				lo = x->kva + x->size;
				break;
			}
			used += x->size;
			first = x;
		}
	}
	for (x = list_next_entry(e, list); !list_entry_is_head(x, &exts, list);
	     x = list_next_entry(x, list)) {
		if (!movable(x)) {
			hi = x->kva;
			break;
		}
		used += x->size;
		last = x;
	}
	if (hi - lo < used + nsize)
		return;
	pst.compactions++;
	t0 = ktime_get_ns();
	dst = lo;
	for (x = first; ; x = list_next_entry(x, list)) {
		if (x->kva != dst && !move_ext(x, dst))
			goto out;
		dst += x->size;
		if (x == e)
			break;
	}
	dst = hi;
	for (x = last; x != e; x = list_prev_entry(x, list)) {
		dst -= x->size;
		if (x->kva != dst && !move_ext(x, dst))
			break;
		dst = x->kva;
	}
out:
	time_moves(t0);
}

/* grow the block at kva to nsize in place, if the space after it is free
 * (or can be made free) */
bool atrans_pool_extend(unsigned long kva, unsigned long nsize)
{
	struct pool_ext *e;
	bool ok = false;

	mutex_lock(&pool_mutex);
	e = ext_find(kva);
	/* the last block: past the pool's end first (cheap), then moves */
	if (e && gap_end(e) - e->kva < nsize && list_is_last(&e->list, &exts))
		pool_grow(nsize - (gap_end(e) - e->kva));
	if (e && gap_end(e) - e->kva < nsize)
		make_room(e, nsize);
	if (e && gap_end(e) - e->kva < nsize && list_is_last(&e->list, &exts))
		pool_grow(nsize - (gap_end(e) - e->kva));
	if (e && gap_end(e) - e->kva >= nsize) {
		pool_used += nsize - e->size;
		e->size = nsize;
		ok = true;
	} else if (e) {
		struct pool_ext *x;
		unsigned int unmov = 0;

		list_for_each_entry(x, &exts, list)
			unmov += x != e && !movable(x);
		pr_info_ratelimited("rp2350-atrans: pool: no room to extend %lu to %lu KiB: free %lu KiB, %u blocks unmovable%s\n",
				    e->size >> 10, nsize >> 10, (pool_end - pool_base - pool_used) >> 10,
				    unmov, movable(e) ? "" : ", itself unmovable");
	}
	mutex_unlock(&pool_mutex);
	return ok;
}

void atrans_pool_trim(unsigned long kva, unsigned long nsize)
{
	struct pool_ext *e;

	mutex_lock(&pool_mutex);
	e = ext_find(kva);
	if (e && nsize < e->size) {
		pool_used -= e->size - nsize;
		e->size = nsize;
	}
	mutex_unlock(&pool_mutex);
}

/* the block at kva now belongs to mm (data) or t (text): it may move */
void atrans_pool_own(unsigned long kva, struct mm_struct *mm, struct atrans_text *t)
{
	struct pool_ext *e;

	mutex_lock(&pool_mutex);
	e = ext_find(kva);
	if (e) {
		e->mm = mm;
		e->text = t;
	}
	mutex_unlock(&pool_mutex);
}

/* For swap's targeted reclaim (rp2350-atrans-swap.c): mark the pool's free
 * pages in cls (one byte per page from start_pfn, 1: free), as it marks
 * free buddy pages. */
void atrans_pool_mark_free(u8 *cls, unsigned long start_pfn, unsigned long end_pfn)
{
	unsigned long s, pfn, e_pfn;
	struct pool_ext *e;

	mutex_lock(&pool_mutex);
	s = pool_base;
	list_for_each_entry(e, &exts, list) {
		for (pfn = virt_to_pfn((void *)s), e_pfn = virt_to_pfn((void *)e->kva); pfn < e_pfn; pfn++)
			if (pfn >= start_pfn && pfn < end_pfn)
				cls[pfn - start_pfn] = 1;
		s = e->kva + e->size;
	}
	if (pool_end > s)
		for (pfn = virt_to_pfn((void *)s), e_pfn = virt_to_pfn((void *)pool_end); pfn < e_pfn; pfn++)
			if (pfn >= start_pfn && pfn < end_pfn)
				cls[pfn - start_pfn] = 1;
	mutex_unlock(&pool_mutex);
}

/*
 * For swap's targeted reclaim: in the pool, compaction makes any free space
 * usable, so a run of `pages` needs only that much free in all. Of the n
 * swap-out candidates (block kva, npages pages), pick blocks in the pool
 * whose swap-out frees enough: the smallest single block that does, else
 * the largest ones first. Sets pick[i]; returns the pages picked (0: the
 * pool has the room already), ULONG_MAX: the pool can't make it.
 */
unsigned long atrans_pool_pick(unsigned long pages, const unsigned long *kva,
			       const unsigned long *npages, unsigned int n, bool *pick)
{
	unsigned long s, free = 0, need, got = 0, best;
	unsigned int i, b;
	struct pool_ext *e;

	if (pool_end == pool_base || pages > (pool_end - pool_base) >> PAGE_SHIFT)
		return ULONG_MAX;
	mutex_lock(&pool_mutex);
	s = pool_base;
	list_for_each_entry(e, &exts, list) {
		free += e->kva - s;
		s = e->kva + e->size;
	}
	free += pool_end - s;
	mutex_unlock(&pool_mutex);
	free >>= PAGE_SHIFT;
	if (free >= pages)
		return 0;
	need = pages - free;
	for (i = 0, b = n; i < n; i++)
		if (atrans_pool_has(kva[i]) && npages[i] >= need &&
		    (b == n || npages[i] < npages[b]))
			b = i;
	if (b < n) {
		pick[b] = true;
		return npages[b];
	}
	while (got < need) {
		for (i = 0, b = n, best = 0; i < n; i++)
			if (!pick[i] && atrans_pool_has(kva[i]) && npages[i] > best) {
				best = npages[i];
				b = i;
			}
		if (b == n)
			break;
		pick[b] = true;
		got += best;
	}
	if (got >= need)
		return got;
	memset(pick, 0, n * sizeof(*pick));
	return ULONG_MAX;
}

/* for block_alloc's failure line: size, free, largest free (0s if busy) */
void atrans_pool_diag(unsigned long *size, unsigned long *free, unsigned long *largest)
{
	unsigned long s, f = 0, l = 0;
	struct pool_ext *e;

	*size = *free = *largest = 0;
	if (pool_end == pool_base || !mutex_trylock(&pool_mutex))
		return;
	s = pool_base;
	list_for_each_entry(e, &exts, list) {
		f += e->kva - s;
		l = max(l, e->kva - s);
		s = e->kva + e->size;
	}
	f += pool_end - s;
	l = max(l, pool_end - s);
	*size = pool_end - pool_base;
	*free = f;
	*largest = l;
	mutex_unlock(&pool_mutex);
}

void atrans_pool_show(struct seq_file *m)
{
	unsigned long s, free = 0, largest = 0, n = 0;
	struct pool_ext *e;

	if (pool_end == pool_base) {
		seq_puts(m, "pool none\n");
		return;
	}
	mutex_lock(&pool_mutex);
	s = pool_base;
	list_for_each_entry(e, &exts, list) {
		free += e->kva - s;
		largest = max(largest, e->kva - s);
		s = e->kva + e->size;
		n++;
	}
	free += pool_end - s;
	largest = max(largest, pool_end - s);
	seq_printf(m, "pool %08lx %lu KiB: %lu blocks, free %lu KiB, largest free %lu KiB\n",
		   pool_base - CONFIG_DRAM_BASE, (pool_end - pool_base) >> 10, n, free >> 10,
		   largest >> 10);
	seq_printf(m, "pool allocs %lu fails %lu compactions %lu moves %lu (%lu KiB), longest %lu us\n",
		   pst.allocs, pst.fails, pst.compactions, pst.moves, pst.moved_bytes >> 10,
		   pst.move_us_max);
	seq_printf(m, "pool lent %lu KiB in %lu (%lu KiB by the shrinker), grown %lu KiB in %lu, min %lu KiB, max %lu KiB\n",
		   pst.lent >> 10, pst.lends, pst.lent_scan >> 10, pst.grown >> 10, pst.grows, pool_min >> 10,
		   (pool_hi - pool_base) >> 10);
	list_for_each_entry(e, &exts, list)
		seq_printf(m, "  %08lx %5lu KiB %s\n", e->kva - CONFIG_DRAM_BASE, e->size >> 10,
			   e->mm ? "data" : e->text ? "text" : "being filled");
	mutex_unlock(&pool_mutex);
}

/* Take the pool from the page allocator (early, while memory is in one
 * piece). block_alloc_run(): rp2350-atrans.c's run allocator. */
void __init atrans_pool_init(void)
{
	unsigned long kva;

	if (!pool_req)
		return;
	kva = block_alloc_run(pool_req);
	if (!kva) {
		pr_warn("rp2350-atrans: no room for a %lu KiB pool\n", pool_req >> 10);
		return;
	}
	pool_base = kva;
	pool_end = kva + pool_req;
	pool_hi = (unsigned long)__va(memblock_end_of_DRAM());
	pr_info("rp2350-atrans: pool of %lu KiB at %#lx\n", pool_req >> 10, kva);
}

/*
 * Take lent space back: every 2 s, while the pool is below its boot size,
 * grow it into the free pages past its end, keeping the page allocator well
 * above its high watermark. (They are free only if nothing unmovable landed
 * there meanwhile; block_alloc() also grows the pool on demand.)
 */
static void pool_back_fn(struct work_struct *w);
static DECLARE_DELAYED_WORK(pool_back, pool_back_fn);

static void pool_back_fn(struct work_struct *w)
{
	unsigned long spare = 0, free = global_zone_page_state(NR_FREE_PAGES), keep = 0;
	struct zone *z;

	for_each_populated_zone(z)
		keep += high_wmark_pages(z);
	keep = 2 * keep + (512UL << 10 >> PAGE_SHIFT);
	if (free > keep)
		spare = (free - keep) << PAGE_SHIFT;
	if (spare && pool_end - pool_base < pool_req && mutex_trylock(&pool_mutex)) {
		pool_grow(min(spare, pool_base + pool_req - pool_end));
		mutex_unlock(&pool_mutex);
	}
	schedule_delayed_work(&pool_back, 2 * HZ);
}

static int __init atrans_pool_shrinker(void)
{
	struct shrinker *sh;

	if (pool_end == pool_base)
		return 0;
	sh = shrinker_alloc(0, "rp2350-atrans-pool");
	if (!sh)
		return -ENOMEM;
	sh->count_objects = pool_count;
	sh->scan_objects = pool_scan;
	sh->seeks = DEFAULT_SEEKS;
	sh->batch = 32;			/* lend 128 KiB at a time */
	shrinker_register(sh);
	schedule_delayed_work(&pool_back, 10 * HZ);
	return 0;
}
late_initcall(atrans_pool_shrinker);
