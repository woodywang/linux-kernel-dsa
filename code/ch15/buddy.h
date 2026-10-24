// SPDX-License-Identifier: MIT
/*
 * buddy.h - a teaching version of the kernel's page allocator (Chapter 15).
 *
 * One zone manages a range of simulated page frame numbers (PFNs).  Free
 * memory is kept in blocks of 2^order pages, on one intrusive list per
 * order and migratetype; allocation splits blocks, freeing merges a block
 * with its buddy (pfn ^ (1 << order)).  Pageblocks carry a migratetype, and
 * an allocation that finds no free block of its own type falls back to
 * another type, claiming a whole pageblock when it can.  A per-CPU front
 * cache keeps small blocks on per-CPU lists and talks to the zone in
 * batches.
 *
 * What it leaves out on purpose: locking and real CPUs (a "cpu" is just an
 * index chosen by the caller; lock_taken counts the times the kernel would
 * take zone->lock), NUMA nodes and zone lists, reclaim and compaction,
 * watermark boosting, the CMA, HIGHATOMIC and ISOLATE migratetypes, guard
 * pages, the auto-tuning of the per-CPU high and batch values, and page
 * shuffling.  Pages hold no data.
 */
#ifndef BUDDY_H
#define BUDDY_H

#include <stdbool.h>
#include <stdio.h>
#include "list.h"

#ifndef MAX_PAGE_ORDER
#define MAX_PAGE_ORDER	10		/* largest block: 2^10 pages */
#endif
#define NR_PAGE_ORDERS	(MAX_PAGE_ORDER + 1)
#ifndef PAGEBLOCK_ORDER
#define PAGEBLOCK_ORDER	9		/* one migratetype per 2^9 pages */
#endif
#define PAGEBLOCK_NR_PAGES	(1UL << PAGEBLOCK_ORDER)
#define PAGE_ALLOC_COSTLY_ORDER	3	/* largest order on per-CPU lists */
#define NR_CPUS_MAX	8
#define PAGE_KB		4		/* 4 KiB pages, for the watermarks */

enum migratetype { MT_UNMOVABLE, MT_MOVABLE, MT_RECLAIMABLE, NR_MT };

/* One per-CPU list per (order, migratetype), as in the kernel. */
#define NR_PCP_LISTS	(NR_MT * (PAGE_ALLOC_COSTLY_ORDER + 1))

struct page {
	struct list_head lru;	/* on a free list or a per-CPU list */
	bool buddy;		/* PageBuddy: first page of a free block */
	unsigned char order;	/* the free block's order, while buddy */
	signed char used_as;	/* migratetype it was allocated as, or -1 */
};

struct free_area {
	struct list_head free_list[NR_MT];
	unsigned long nr_free;	/* free blocks of this order, all types */
};

struct per_cpu_pages {
	int count;		/* pages on all lists */
	int high;		/* drain when count reaches this */
	int batch;		/* pages moved per refill or drain */
	struct list_head lists[NR_PCP_LISTS];
};

struct zone {
	unsigned long start_pfn, end_pfn;	/* [start_pfn, end_pfn) */
	struct page *pages;			/* pages[pfn - start_pfn] */
	unsigned char *pageblock_mt;		/* one byte per pageblock */
	struct free_area free_area[NR_PAGE_ORDERS];
	unsigned long nr_free;			/* pages in the free lists */
	unsigned long wmark_min, wmark_low, wmark_high;
	bool group_by_mobility;
	int nr_cpus;
	struct per_cpu_pages pcp[NR_CPUS_MAX];
	unsigned long lock_taken;		/* simulated zone->lock acquisitions */
	unsigned long fallbacks, claims;	/* allocations from another type */
};

/* batch < 0 computes it from the zone size as the kernel does; 0 disables
 * the per-CPU lists. */
void zone_init(struct zone *z, unsigned long start_pfn, unsigned long nr_pages,
	       int nr_cpus, int batch, bool group_by_mobility);
void zone_destroy(struct zone *z);

long alloc_pages(struct zone *z, int cpu, unsigned int order, int mt);
void free_pages(struct zone *z, int cpu, unsigned long pfn, unsigned int order);
void drain_pages(struct zone *z, int cpu);
void drain_all_pages(struct zone *z);

int get_pageblock_mt(const struct zone *z, unsigned long pfn);
bool zone_watermark_ok(const struct zone *z, unsigned int order,
		       unsigned long mark);
int unusable_free_index(const struct zone *z, unsigned int order);
void print_buddyinfo(const struct zone *z, FILE *f);

static inline struct page *pfn_to_page(const struct zone *z, unsigned long pfn)
{
	return &z->pages[pfn - z->start_pfn];
}

static inline unsigned long page_to_pfn(const struct zone *z,
					const struct page *p)
{
	return z->start_pfn + (unsigned long)(p - z->pages);
}

#endif
