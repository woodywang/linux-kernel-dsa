// SPDX-License-Identifier: MIT
/*
 * test_buddy.c - randomized test of buddy.c against a simple model.
 *
 *   make check            (or: ./test_buddy [iterations] [seed])
 *
 * The model is one byte per page: allocated or not.  Random allocations
 * (all orders, all migratetypes, several "CPUs"), frees, and drains are
 * applied to both, and every few steps the allocator's structures are
 * walked and checked:
 *   - every free block is aligned, inside the zone, marked as a buddy of
 *     its order, and on the list of its pageblock's migratetype;
 *   - free blocks, per-CPU pages, and allocated pages never overlap, and
 *     together cover the zone exactly (no page lost or duplicated);
 *   - no free block has a free buddy of the same order (merging is
 *     complete), and the per-order and per-CPU counters are right.
 * An allocation may fail only if, after draining the per-CPU lists, the
 * model has no free naturally aligned run of the requested size.  After
 * everything is freed, the zone must be back to its initial free lists.
 */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "buddy.h"

/* Zone sizes shrink with MAX_PAGE_ORDER so that small builds fill up too. */
#define SHRINK	(MAX_PAGE_ORDER < 10 ? 10 - MAX_PAGE_ORDER : 0)

static uint64_t rng = 1;

static uint64_t rnd(void)			/* xorshift64* */
{
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return rng * 2685821657736338717ULL;
}

struct alloc {
	unsigned long pfn;
	unsigned int order;
	int mt;
};

static struct zone z;
static unsigned char *model;		/* 1 = allocated */
static char *seen;			/* scratch for check(): 'A', 'B', 'P' */
static struct alloc *live;
static int nlive;
static unsigned long nr_pages;
static unsigned long initial[NR_PAGE_ORDERS];

static void mark(unsigned long pfn, unsigned long n, char what)
{
	for (unsigned long i = 0; i < n; i++) {
		unsigned long idx = pfn + i - z.start_pfn;

		assert(pfn + i >= z.start_pfn && pfn + i < z.end_pfn);
		assert(seen[idx] == 0);		/* no overlap */
		seen[idx] = what;
	}
}

static void check(void)
{
	unsigned long total = 0;

	memset(seen, 0, nr_pages);
	/* Allocated pages, from the model. */
	for (int i = 0; i < nlive; i++)
		mark(live[i].pfn, 1UL << live[i].order, 'A');

	/* Free lists. */
	for (unsigned int o = 0; o < NR_PAGE_ORDERS; o++) {
		unsigned long n = 0;

		for (int mt = 0; mt < NR_MT; mt++) {
			struct page *p;

			list_for_each_entry(p, &z.free_area[o].free_list[mt], lru) {
				unsigned long pfn = page_to_pfn(&z, p);

				assert(p->buddy && p->order == o);
				assert((pfn & ((1UL << o) - 1)) == 0);
				assert(pfn + (1UL << o) <= z.end_pfn);
				for (unsigned long b = pfn; b < pfn + (1UL << o);
				     b += PAGEBLOCK_NR_PAGES)
					assert(get_pageblock_mt(&z, b) == mt);
				mark(pfn, 1UL << o, 'B');
				n++;
			}
		}
		assert(n == z.free_area[o].nr_free);
		total += n << o;
	}
	assert(total == z.nr_free);

	/* Per-CPU lists. */
	for (int cpu = 0; cpu < z.nr_cpus; cpu++) {
		int count = 0;

		for (int i = 0; i < NR_PCP_LISTS; i++) {
			unsigned int o = (unsigned int)i / NR_MT;
			struct page *p;

			list_for_each_entry(p, &z.pcp[cpu].lists[i], lru) {
				unsigned long pfn = page_to_pfn(&z, p);

				assert(!p->buddy);
				assert((pfn & ((1UL << o) - 1)) == 0);
				mark(pfn, 1UL << o, 'P');
				count += 1 << o;
			}
		}
		assert(count == z.pcp[cpu].count);
	}

	/* Every page is exactly one of A, B, P; the model agrees on A. */
	for (unsigned long i = 0; i < nr_pages; i++) {
		assert(seen[i] != 0);
		assert((seen[i] == 'A') == (model[i] == 1));
		assert((seen[i] == 'A') == (z.pages[i].used_as >= 0));
		if (seen[i] != 'B')
			assert(!z.pages[i].buddy);
	}

	/* Merging is complete: no free block has a free buddy of its order. */
	for (unsigned int o = 0; o < MAX_PAGE_ORDER; o++)
		for (int mt = 0; mt < NR_MT; mt++) {
			struct page *p;

			list_for_each_entry(p, &z.free_area[o].free_list[mt], lru) {
				unsigned long b = page_to_pfn(&z, p) ^ (1UL << o);

				if (b >= z.start_pfn && b < z.end_pfn) {
					struct page *bp = pfn_to_page(&z, b);

					assert(!(bp->buddy && bp->order == o));
				}
			}
		}
}

/* Does the model have a free, naturally aligned run of 2^order pages? */
static bool model_has_free_run(unsigned int order)
{
	unsigned long n = 1UL << order;
	unsigned long pfn = (z.start_pfn + n - 1) & ~(n - 1);

	for (; pfn + n <= z.end_pfn; pfn += n) {
		unsigned long i;

		for (i = 0; i < n; i++)
			if (model[pfn - z.start_pfn + i])
				break;
		if (i == n)
			return true;
	}
	return false;
}

static unsigned int random_order(void)
{
	unsigned int r = (unsigned int)(rnd() % 100);

	if (r < 55)
		return 0;
	if (r < 75)
		return 1 + (unsigned int)(rnd() % 3);	/* per-CPU orders */
	return (unsigned int)(rnd() % (MAX_PAGE_ORDER + 2));	/* incl. too big */
}

static void free_one(int i, int cpu)
{
	struct alloc a = live[i];

	for (unsigned long k = 0; k < 1UL << a.order; k++)
		model[a.pfn - z.start_pfn + k] = 0;
	free_pages(&z, cpu, a.pfn, a.order);
	live[i] = live[--nlive];
}

static void run(unsigned long start, unsigned long pages, int cpus, int batch,
		bool group, long iters)
{
	long fails = 0, fallbacks;

	nr_pages = pages;
	zone_init(&z, start, pages, cpus, batch, group);
	model = calloc(pages, 1);
	seen = malloc(pages);
	live = malloc(pages * sizeof(*live));
	assert(model && seen && live);
	nlive = 0;
	for (unsigned int o = 0; o < NR_PAGE_ORDERS; o++)
		initial[o] = z.free_area[o].nr_free;
	check();

	for (long it = 0; it < iters; it++) {
		int cpu = (int)(rnd() % (uint64_t)cpus);
		unsigned int r = (unsigned int)(rnd() % 1000);

		if (r < 520 || nlive == 0) {
			unsigned int order = random_order();
			int mt = (int)(rnd() % NR_MT);
			long pfn = alloc_pages(&z, cpu, order, mt);

			if (order > MAX_PAGE_ORDER) {
				assert(pfn < 0);	/* too big: refused */
			} else if (pfn < 0) {
				fails++;
				/* All per-CPU pages were drained before failing. */
				for (int c = 0; c < cpus; c++)
					assert(z.pcp[c].count == 0);
				assert(!model_has_free_run(order));
			} else {
				unsigned long p = (unsigned long)pfn;

				assert((p & ((1UL << order) - 1)) == 0);
				assert(p >= z.start_pfn &&
				       p + (1UL << order) <= z.end_pfn);
				for (unsigned long k = 0; k < 1UL << order; k++) {
					assert(model[p - z.start_pfn + k] == 0);
					model[p - z.start_pfn + k] = 1;
				}
				live[nlive++] = (struct alloc){ p, order, mt };
			}
		} else if (r < 980) {
			free_one((int)(rnd() % (uint64_t)nlive), cpu);
		} else if (r < 995) {
			drain_pages(&z, cpu);
		} else {
			while (nlive)		/* free everything */
				free_one(nlive - 1, (int)(rnd() % (uint64_t)cpus));
			drain_all_pages(&z);
			check();
			for (unsigned int o = 0; o < NR_PAGE_ORDERS; o++)
				assert(z.free_area[o].nr_free == initial[o]);
		}
		if (it % 61 == 0)
			check();
	}
	check();
	fallbacks = (long)z.fallbacks;
	printf("  zone [%lu, %lu) cpus %d batch %d group %d: %ld ops, "
	       "%ld failed allocs, %ld fallbacks, %lu claims\n",
	       start, start + pages, cpus, z.pcp[0].batch, group, iters, fails,
	       fallbacks, z.claims);
	zone_destroy(&z);
	free(model);
	free(seen);
	free(live);
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 100000;

	rng = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	if (!rng)
		rng = 1;
	printf("test_buddy: MAX_PAGE_ORDER %d, PAGEBLOCK_ORDER %d\n",
	       MAX_PAGE_ORDER, PAGEBLOCK_ORDER);
	/* Unaligned start and odd size: blocks at the edges are clipped. */
	run(1000, 5000 >> SHRINK, 4, 7, true, iters);
	run(3 << MAX_PAGE_ORDER, 3UL << MAX_PAGE_ORDER, 2, 15, true, iters);
	run(77, 3001 >> SHRINK, 3, 0, true, iters);	/* no per-CPU lists */
	run(12345, 4096 >> SHRINK, 4, -1, false, iters); /* no mobility groups */
	printf("test_buddy: all checks passed\n");
	return 0;
}
