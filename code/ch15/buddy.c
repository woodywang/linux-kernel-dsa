// SPDX-License-Identifier: MIT
/*
 * buddy.c - a teaching version of the kernel's page allocator.
 * See buddy.h for what it models and what it leaves out.  Function names
 * follow mm/page_alloc.c so that the real code is easy to find.
 */
#include <assert.h>
#include <stdlib.h>
#include "buddy.h"

/* ------------------------------------------------------------------ */
/* Pageblocks and free lists                                           */

static unsigned long pageblock_index(const struct zone *z, unsigned long pfn)
{
	return (pfn >> PAGEBLOCK_ORDER) - (z->start_pfn >> PAGEBLOCK_ORDER);
}

int get_pageblock_mt(const struct zone *z, unsigned long pfn)
{
	return z->pageblock_mt[pageblock_index(z, pfn)];
}

static void set_pageblock_mt(struct zone *z, unsigned long pfn, int mt)
{
	z->pageblock_mt[pageblock_index(z, pfn)] = (unsigned char)mt;
}

/* Retype every pageblock that a block of 2^order pages covers. */
static void change_pageblock_range(struct zone *z, unsigned long pfn,
				   unsigned int order, int mt)
{
	unsigned long end = pfn + (1UL << order);

	for (; pfn < end; pfn += PAGEBLOCK_NR_PAGES)
		set_pageblock_mt(z, pfn, mt);
}

static void add_to_free_list(struct zone *z, unsigned long pfn,
			     unsigned int order, int mt, bool tail)
{
	struct page *page = pfn_to_page(z, pfn);
	struct list_head *list = &z->free_area[order].free_list[mt];

	page->buddy = true;
	page->order = (unsigned char)order;
	if (tail)
		list_add_tail(&page->lru, list);
	else
		list_add(&page->lru, list);
	z->free_area[order].nr_free++;
	z->nr_free += 1UL << order;
}

static void del_from_free_list(struct zone *z, unsigned long pfn,
			       unsigned int order)
{
	struct page *page = pfn_to_page(z, pfn);

	list_del(&page->lru);
	page->buddy = false;
	z->free_area[order].nr_free--;
	z->nr_free -= 1UL << order;
}

/* ------------------------------------------------------------------ */
/* Freeing: merge with the buddy as long as it is free                 */

static inline unsigned long __find_buddy_pfn(unsigned long pfn,
					     unsigned int order)
{
	return pfn ^ (1UL << order);
}

/* Is the block at pfn a free block of exactly this order in this zone? */
static bool page_is_buddy(const struct zone *z, unsigned long pfn,
			  unsigned int order)
{
	const struct page *page;

	if (pfn < z->start_pfn || pfn >= z->end_pfn)
		return false;			/* outside the zone */
	page = pfn_to_page(z, pfn);
	return page->buddy && page->order == order;
}

/*
 * If the block we just built has a free buddy one order up, the two are
 * likely to merge soon; queue it at the cold end so it is not reused first.
 */
static bool buddy_merge_likely(const struct zone *z, unsigned long pfn,
			       unsigned int order)
{
	unsigned long parent;

	if (order >= MAX_PAGE_ORDER - 1)
		return false;
	parent = pfn & ~(1UL << order);	/* our block plus its buddy */
	return page_is_buddy(z, __find_buddy_pfn(parent, order + 1), order + 1);
}

static void __free_one_page(struct zone *z, unsigned long pfn,
			    unsigned int order, int mt)
{
	while (order < MAX_PAGE_ORDER) {
		unsigned long buddy_pfn = __find_buddy_pfn(pfn, order);

		if (!page_is_buddy(z, buddy_pfn, order))
			break;
		del_from_free_list(z, buddy_pfn, order);
		if (order >= PAGEBLOCK_ORDER &&
		    get_pageblock_mt(z, buddy_pfn) != mt)
			change_pageblock_range(z, buddy_pfn, order, mt);
		pfn &= buddy_pfn;		/* the merged block starts here */
		order++;
	}
	add_to_free_list(z, pfn, order, mt, buddy_merge_likely(z, pfn, order));
}

/* ------------------------------------------------------------------ */
/* Allocation: take the smallest block that fits and split it          */

/* Give back the upper halves of [pfn, pfn + 2^high) down to order low. */
static void expand(struct zone *z, unsigned long pfn, unsigned int low,
		   unsigned int high, int mt)
{
	while (high > low) {
		high--;
		add_to_free_list(z, pfn + (1UL << high), high, mt, false);
	}
}

static long __rmqueue_smallest(struct zone *z, unsigned int order, int mt)
{
	for (unsigned int o = order; o < NR_PAGE_ORDERS; o++) {
		struct list_head *list = &z->free_area[o].free_list[mt];
		unsigned long pfn;

		if (list_empty(list))
			continue;
		pfn = page_to_pfn(z, list_first_entry(list, struct page, lru));
		del_from_free_list(z, pfn, o);
		expand(z, pfn, order, o, mt);
		return (long)pfn;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* Falling back to another migratetype                                 */

static const int fallbacks[NR_MT][NR_MT - 1] = {
	[MT_UNMOVABLE]   = { MT_RECLAIMABLE, MT_MOVABLE },
	[MT_MOVABLE]     = { MT_RECLAIMABLE, MT_UNMOVABLE },
	[MT_RECLAIMABLE] = { MT_UNMOVABLE, MT_MOVABLE },
};

/* First fallback type with a free block of this order, or -1. */
static int find_suitable_fallback(const struct zone *z, unsigned int order,
				  int start_mt)
{
	for (int i = 0; i < NR_MT - 1; i++) {
		int mt = fallbacks[start_mt][i];

		if (!list_empty(&z->free_area[order].free_list[mt]))
			return mt;
	}
	return -1;
}

static bool should_try_claim_block(unsigned int order, int start_mt)
{
	if (order >= PAGEBLOCK_ORDER / 2)
		return true;		/* the block is probably mostly free */
	return start_mt != MT_MOVABLE;	/* don't let unmovable pages pollute */
}

/* Move every free block inside one pageblock to new_mt's lists. */
static void move_freepages_block(struct zone *z, unsigned long start, int new_mt)
{
	unsigned long pfn = start;

	while (pfn < start + PAGEBLOCK_NR_PAGES) {
		struct page *page = pfn_to_page(z, pfn);

		if (!page->buddy) {
			pfn++;
			continue;
		}
		list_move(&page->lru, &z->free_area[page->order].free_list[new_mt]);
		pfn += 1UL << page->order;
	}
}

static long try_to_claim_block(struct zone *z, unsigned long pfn,
			       unsigned int cur, unsigned int order,
			       int start_mt, int block_mt)
{
	unsigned long start = pfn & ~(PAGEBLOCK_NR_PAGES - 1);
	unsigned long free = 0, alike = 0;

	if (cur >= PAGEBLOCK_ORDER) {		/* whole pageblocks: take them */
		del_from_free_list(z, pfn, cur);
		change_pageblock_range(z, pfn, cur, start_mt);
		expand(z, pfn, order, cur, start_mt);
		z->claims++;
		return (long)pfn;
	}
	if (start < z->start_pfn || start + PAGEBLOCK_NR_PAGES > z->end_pfn)
		return -1;			/* pageblock crosses the zone edge */

	/* Count free pages, and used pages compatible with start_mt. */
	for (unsigned long p = start; p < start + PAGEBLOCK_NR_PAGES; ) {
		const struct page *page = pfn_to_page(z, p);

		if (page->buddy) {
			free += 1UL << page->order;
			p += 1UL << page->order;
			continue;
		}
		if (start_mt == MT_MOVABLE)
			alike += page->used_as == MT_MOVABLE;
		else if (block_mt == MT_MOVABLE)
			alike += page->used_as >= 0 && page->used_as != MT_MOVABLE;
		p++;
	}
	if (free + alike < PAGEBLOCK_NR_PAGES / 2)
		return -1;			/* not worth converting */
	move_freepages_block(z, start, start_mt);
	set_pageblock_mt(z, start, start_mt);
	z->claims++;
	return __rmqueue_smallest(z, order, start_mt);
}

/* Largest blocks first: try to convert a whole pageblock to start_mt. */
static long __rmqueue_claim(struct zone *z, unsigned int order, int start_mt)
{
	for (int o = MAX_PAGE_ORDER; o >= (int)order; o--) {
		int mt;
		long pfn;

		if (!should_try_claim_block(o, start_mt))
			break;
		mt = find_suitable_fallback(z, o, start_mt);
		if (mt < 0)
			continue;
		pfn = page_to_pfn(z, list_first_entry(
			&z->free_area[o].free_list[mt], struct page, lru));
		pfn = try_to_claim_block(z, pfn, o, order, start_mt, mt);
		if (pfn >= 0)
			return pfn;
	}
	return -1;
}

/* Smallest block first: borrow one block, leave the pageblock's type. */
static long __rmqueue_steal(struct zone *z, unsigned int order, int start_mt)
{
	for (unsigned int o = order; o < NR_PAGE_ORDERS; o++) {
		int mt = find_suitable_fallback(z, o, start_mt);
		unsigned long pfn;

		if (mt < 0)
			continue;
		pfn = page_to_pfn(z, list_first_entry(
			&z->free_area[o].free_list[mt], struct page, lru));
		del_from_free_list(z, pfn, o);
		expand(z, pfn, order, o, mt);
		return (long)pfn;
	}
	return -1;
}

static long __rmqueue(struct zone *z, unsigned int order, int mt)
{
	long pfn = __rmqueue_smallest(z, order, mt);

	if (pfn >= 0)
		return pfn;
	z->fallbacks++;
	pfn = __rmqueue_claim(z, order, mt);
	if (pfn < 0)
		pfn = __rmqueue_steal(z, order, mt);
	return pfn;
}

/* ------------------------------------------------------------------ */
/* Per-CPU lists: a front cache that talks to the zone in batches      */

static unsigned int order_to_pindex(int mt, unsigned int order)
{
	return NR_MT * order + (unsigned int)mt;
}

/* One trip to the zone: up to count blocks onto a per-CPU list. */
static int rmqueue_bulk(struct zone *z, unsigned int order, int count,
			struct list_head *list, int mt)
{
	int i;

	z->lock_taken++;
	for (i = 0; i < count; i++) {
		long pfn = __rmqueue(z, order, mt);

		if (pfn < 0)
			break;
		list_add_tail(&pfn_to_page(z, (unsigned long)pfn)->lru, list);
	}
	return i;
}

/* One trip to the zone: give back count pages, coldest first. */
static void free_pcppages_bulk(struct zone *z, int count,
			       struct per_cpu_pages *pcp, unsigned int pindex)
{
	z->lock_taken++;
	while (count > 0 && pcp->count > 0) {
		struct list_head *list;
		struct page *page;
		unsigned int order;
		unsigned long pfn;

		do {				/* round-robin over the lists */
			pindex = (pindex + 1) % NR_PCP_LISTS;
			list = &pcp->lists[pindex];
		} while (list_empty(list));
		order = pindex / NR_MT;
		page = list_last_entry(list, struct page, lru);
		list_del(&page->lru);
		pfn = page_to_pfn(z, page);
		pcp->count -= 1 << order;
		count -= 1 << order;
		__free_one_page(z, pfn, order, get_pageblock_mt(z, pfn));
	}
}

static long rmqueue_pcplist(struct zone *z, int cpu, unsigned int order,
			    int mt)
{
	struct per_cpu_pages *pcp = &z->pcp[cpu];
	struct list_head *list = &pcp->lists[order_to_pindex(mt, order)];
	struct page *page;

	if (list_empty(list)) {
		int batch = pcp->batch >> order;

		if (batch < 2)
			batch = 2;
		pcp->count += rmqueue_bulk(z, order, batch, list, mt) << order;
		if (list_empty(list))
			return -1;
	}
	page = list_first_entry(list, struct page, lru);	/* hottest */
	list_del(&page->lru);
	pcp->count -= 1 << order;
	return (long)page_to_pfn(z, page);
}

static void free_pcp(struct zone *z, int cpu, unsigned long pfn,
		     unsigned int order)
{
	struct per_cpu_pages *pcp = &z->pcp[cpu];
	unsigned int pindex = order_to_pindex(get_pageblock_mt(z, pfn), order);

	list_add(&pfn_to_page(z, pfn)->lru, &pcp->lists[pindex]);
	pcp->count += 1 << order;
	if (pcp->count >= pcp->high)
		free_pcppages_bulk(z, pcp->batch, pcp, pindex - 1);
}

void drain_pages(struct zone *z, int cpu)
{
	struct per_cpu_pages *pcp = &z->pcp[cpu];

	if (pcp->count)
		free_pcppages_bulk(z, pcp->count, pcp, NR_PCP_LISTS - 1);
}

void drain_all_pages(struct zone *z)
{
	for (int cpu = 0; cpu < z->nr_cpus; cpu++)
		drain_pages(z, cpu);
}

/* ------------------------------------------------------------------ */
/* The public interface                                                */

static bool pcp_allowed(const struct zone *z, int cpu, unsigned int order)
{
	return order <= PAGE_ALLOC_COSTLY_ORDER && z->pcp[cpu].batch > 0;
}

static long rmqueue(struct zone *z, int cpu, unsigned int order, int mt)
{
	if (pcp_allowed(z, cpu, order))
		return rmqueue_pcplist(z, cpu, order, mt);
	z->lock_taken++;
	return __rmqueue(z, order, mt);
}

long alloc_pages(struct zone *z, int cpu, unsigned int order, int mt)
{
	long pfn;

	if (order > MAX_PAGE_ORDER)
		return -1;
	if (!z->group_by_mobility)
		mt = MT_UNMOVABLE;		/* as the kernel does */
	pfn = rmqueue(z, cpu, order, mt);
	if (pfn < 0) {
		/* Pages may be sitting on per-CPU lists: drain, retry once. */
		drain_all_pages(z);
		pfn = rmqueue(z, cpu, order, mt);
	}
	if (pfn >= 0)
		for (unsigned long i = 0; i < 1UL << order; i++)
			pfn_to_page(z, (unsigned long)pfn + i)->used_as = (signed char)mt;
	return pfn;
}

void free_pages(struct zone *z, int cpu, unsigned long pfn, unsigned int order)
{
	for (unsigned long i = 0; i < 1UL << order; i++)
		pfn_to_page(z, pfn + i)->used_as = -1;
	if (pcp_allowed(z, cpu, order)) {
		free_pcp(z, cpu, pfn, order);
		return;
	}
	z->lock_taken++;
	__free_one_page(z, pfn, order, get_pageblock_mt(z, pfn));
}

/* ------------------------------------------------------------------ */
/* Watermarks and statistics                                           */

/* Order-0 check against the mark, then: is any block big enough? */
bool zone_watermark_ok(const struct zone *z, unsigned int order,
		       unsigned long mark)
{
	long free = (long)z->nr_free - ((1L << order) - 1);

	if (free <= (long)mark)
		return false;
	if (order == 0)
		return true;
	for (unsigned int o = order; o < NR_PAGE_ORDERS; o++)
		if (z->free_area[o].nr_free)
			return true;
	return false;
}

static unsigned long int_sqrt(unsigned long x)
{
	unsigned long r = 0;

	while ((r + 1) * (r + 1) <= x)
		r++;
	return r;
}

/* min_free_kbytes = sqrt(16 * kbytes); gaps of max(min/4, 0.1%). */
static void setup_watermarks(struct zone *z)
{
	unsigned long managed = z->end_pfn - z->start_pfn;
	unsigned long min_kb = int_sqrt(managed * PAGE_KB * 16), gap;

	if (min_kb < 128)
		min_kb = 128;
	if (min_kb > 262144)
		min_kb = 262144;
	z->wmark_min = min_kb / PAGE_KB;
	gap = managed * 10 / 10000;		/* watermark_scale_factor 10 */
	if (gap < z->wmark_min / 4)
		gap = z->wmark_min / 4;
	z->wmark_low = z->wmark_min + gap;
	z->wmark_high = z->wmark_low + gap;
}

/* Per mille of free memory in blocks smaller than 2^order pages. */
int unusable_free_index(const struct zone *z, unsigned int order)
{
	unsigned long free = 0, suitable = 0;

	for (unsigned int o = 0; o < NR_PAGE_ORDERS; o++) {
		free += z->free_area[o].nr_free << o;
		if (o >= order)
			suitable += z->free_area[o].nr_free << (o - order);
	}
	if (free == 0)
		return 1000;
	return (int)((free - (suitable << order)) * 1000 / free);
}

/* The format of one line of /proc/buddyinfo. */
void print_buddyinfo(const struct zone *z, FILE *f)
{
	fprintf(f, "Node 0, zone %8s ", "Normal");
	for (unsigned int o = 0; o < NR_PAGE_ORDERS; o++)
		fprintf(f, "%6lu ", z->free_area[o].nr_free);
	fputc('\n', f);
}

/* ------------------------------------------------------------------ */
/* Setting up and tearing down                                         */

/* The kernel's zone_batchsize(): ~0.1% of the zone or 1 MiB, then 2^n - 1. */
static int zone_batchsize(unsigned long managed)
{
	unsigned long batch = managed >> 10;

	if (batch > 1024 / PAGE_KB)
		batch = 1024 / PAGE_KB;
	batch /= 4;
	if (batch < 1)
		batch = 1;
	batch += batch / 2;
	while (batch & (batch - 1))		/* round down to a power of 2 */
		batch &= batch - 1;
	return batch - 1 > 0 ? (int)batch - 1 : 1;
}

void zone_init(struct zone *z, unsigned long start_pfn, unsigned long nr_pages,
	       int nr_cpus, int batch, bool group_by_mobility)
{
	unsigned long nr_blocks;

	assert(nr_pages > 0 && nr_cpus > 0 && nr_cpus <= NR_CPUS_MAX);
	*z = (struct zone){ .start_pfn = start_pfn,
			    .end_pfn = start_pfn + nr_pages,
			    .group_by_mobility = group_by_mobility,
			    .nr_cpus = nr_cpus };
	z->pages = calloc(nr_pages, sizeof(*z->pages));
	nr_blocks = ((z->end_pfn - 1) >> PAGEBLOCK_ORDER) -
		    (start_pfn >> PAGEBLOCK_ORDER) + 1;
	z->pageblock_mt = malloc(nr_blocks);
	assert(z->pages && z->pageblock_mt);
	for (unsigned long i = 0; i < nr_blocks; i++)
		z->pageblock_mt[i] = MT_MOVABLE;	/* as at boot */
	for (unsigned long i = 0; i < nr_pages; i++)
		z->pages[i].used_as = -1;
	for (unsigned int o = 0; o < NR_PAGE_ORDERS; o++)
		for (int mt = 0; mt < NR_MT; mt++)
			INIT_LIST_HEAD(&z->free_area[o].free_list[mt]);
	setup_watermarks(z);
	if (batch < 0)
		batch = zone_batchsize(nr_pages);
	for (int cpu = 0; cpu < nr_cpus; cpu++) {
		struct per_cpu_pages *pcp = &z->pcp[cpu];
		int high = (int)(z->wmark_low / (unsigned long)nr_cpus);

		pcp->batch = batch;
		pcp->high = high > batch * 4 ? high : batch * 4;
		for (int i = 0; i < NR_PCP_LISTS; i++)
			INIT_LIST_HEAD(&pcp->lists[i]);
	}

	/* Hand all pages to the allocator in the largest aligned blocks. */
	for (unsigned long pfn = start_pfn; pfn < z->end_pfn; ) {
		unsigned int o = MAX_PAGE_ORDER;

		while (o && ((pfn & ((1UL << o) - 1)) ||
			     pfn + (1UL << o) > z->end_pfn))
			o--;
		__free_one_page(z, pfn, o, MT_MOVABLE);
		pfn += 1UL << o;
	}
}

void zone_destroy(struct zone *z)
{
	free(z->pages);
	free(z->pageblock_mt);
	z->pages = NULL;
	z->pageblock_mt = NULL;
}
