// SPDX-License-Identifier: MIT
/*
 * buddy_demo.c - the demonstrations quoted in Chapter 15.
 *
 *   1. Splitting and merging in a 16-page zone.
 *   2. Watermarks and per-CPU list sizes for a 256 MiB zone.
 *   3. How often the zone lock is taken with and without per-CPU lists.
 *   4. Fragmentation after a mixed workload, with and without grouping
 *      pages by mobility.
 *
 * All randomness comes from a fixed seed, so the output is reproducible.
 */
#include <stdint.h>
#include <stdlib.h>
#include "buddy.h"

static uint64_t rng = 42;

static uint64_t rnd(void)			/* xorshift64* */
{
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return rng * 2685821657736338717ULL;
}

/* Print "o<order>:<pfn>,<pfn>" for every non-empty order. */
static void show_free(const struct zone *z, const char *what)
{
	printf("  %-16s", what);
	for (unsigned int o = 0; o < NR_PAGE_ORDERS; o++) {
		const char *sep = "";
		struct page *p;

		if (!z->free_area[o].nr_free)
			continue;
		printf(" o%u:", o);
		for (int mt = 0; mt < NR_MT; mt++)
			list_for_each_entry(p, &z->free_area[o].free_list[mt], lru) {
				printf("%s%lu", sep, page_to_pfn(z, p));
				sep = ",";
			}
	}
	putchar('\n');
}

static void split_and_merge(void)
{
	struct zone z;
	char buf[32];
	long a, b, c;

	printf("1. A 16-page zone (free blocks as o<order>:<first pfn>)\n");
	zone_init(&z, 0, 16, 1, 0, true);
	show_free(&z, "start");
	a = alloc_pages(&z, 0, 0, MT_MOVABLE);
	snprintf(buf, sizeof(buf), "alloc o0 -> %ld", a);
	show_free(&z, buf);
	b = alloc_pages(&z, 0, 1, MT_MOVABLE);
	snprintf(buf, sizeof(buf), "alloc o1 -> %ld", b);
	show_free(&z, buf);
	c = alloc_pages(&z, 0, 0, MT_MOVABLE);
	snprintf(buf, sizeof(buf), "alloc o0 -> %ld", c);
	show_free(&z, buf);
	free_pages(&z, 0, (unsigned long)a, 0);
	show_free(&z, "free 0 (o0)");
	free_pages(&z, 0, (unsigned long)c, 0);
	show_free(&z, "free 1 (o0)");
	free_pages(&z, 0, (unsigned long)b, 1);
	show_free(&z, "free 2 (o1)");
	zone_destroy(&z);
}

static void sizes(void)
{
	struct zone z;

	zone_init(&z, 0, 65536, 4, -1, true);
	printf("\n2. A 256 MiB zone (65536 pages of 4 KiB), 4 CPUs\n");
	printf("  watermarks: min %lu, low %lu, high %lu pages\n",
	       z.wmark_min, z.wmark_low, z.wmark_high);
	printf("  per-CPU lists: batch %d, high %d pages\n",
	       z.pcp[0].batch, z.pcp[0].high);
	zone_destroy(&z);
}

/* Random single-page allocations and frees from 4 CPUs. */
static unsigned long churn(int batch)
{
	enum { OPS = 200000, LIVE = 4096 };
	static long live[LIVE];
	struct zone z;
	unsigned long taken;
	int n = 0;

	rng = 7;
	zone_init(&z, 0, 65536, 4, batch, true);
	for (int i = 0; i < OPS; i++) {
		int cpu = (int)(rnd() % 4);

		if (n < LIVE && (n == 0 || rnd() % 2)) {
			live[n++] = alloc_pages(&z, cpu, 0, MT_MOVABLE);
		} else {
			int k = (int)(rnd() % (uint64_t)n);

			free_pages(&z, cpu, (unsigned long)live[k], 0);
			live[k] = live[--n];
		}
	}
	taken = z.lock_taken;
	zone_destroy(&z);
	return taken;
}

static void lock_counts(void)
{
	printf("\n3. 200000 single-page allocations and frees on 4 CPUs\n");
	printf("  zone lock taken: %lu times without per-CPU lists\n", churn(0));
	printf("  zone lock taken: %lu times with per-CPU lists\n", churn(-1));
}

/*
 * Fill the zone to about 75% with single pages, one in ten of them
 * unmovable, churn for a while, then free every movable page, as if
 * reclaim or migration had removed them all.  What is left is pinned by
 * the unmovable pages.
 */
static void fragmentation(bool group)
{
	enum { PAGES = 65536, TARGET = PAGES * 3 / 4, OPS = 300000 };
	static long pfn[PAGES];
	static int mt[PAGES];
	char map[PAGES / PAGEBLOCK_NR_PAGES];
	struct zone z;
	unsigned long polluted = 0, unmovable = 0, huge = 0;
	int n = 0;

	rng = 2026;
	zone_init(&z, 0, PAGES, 4, -1, group);
	for (int i = 0; i < OPS; i++) {
		int cpu = (int)(rnd() % 4);

		if (n < TARGET && (i < TARGET || rnd() % 2)) {
			mt[n] = rnd() % 10 ? MT_MOVABLE : MT_UNMOVABLE;
			pfn[n] = alloc_pages(&z, cpu, 0, mt[n]);
			n++;
		} else {
			int k = (int)(rnd() % (uint64_t)n);

			free_pages(&z, cpu, (unsigned long)pfn[k], 0);
			pfn[k] = pfn[--n];
			mt[k] = mt[n];
		}
	}
	for (int k = n - 1; k >= 0; k--) {
		if (mt[k] == MT_MOVABLE) {
			free_pages(&z, 0, (unsigned long)pfn[k], 0);
			pfn[k] = pfn[--n];
			mt[k] = mt[n];
		}
	}
	drain_all_pages(&z);

	/* One character per pageblock: U if any page in it is still in use. */
	for (unsigned long b = 0; b < PAGES; b += PAGEBLOCK_NR_PAGES) {
		bool has = false;

		for (unsigned long p = b; p < b + PAGEBLOCK_NR_PAGES; p++)
			has |= pfn_to_page(&z, p)->used_as >= 0;
		polluted += has;
		map[b / PAGEBLOCK_NR_PAGES] = has ? 'U' : '.';
	}
	unmovable = (unsigned long)n;
	for (unsigned int o = PAGEBLOCK_ORDER; o < NR_PAGE_ORDERS; o++)
		huge += z.free_area[o].nr_free << (o - PAGEBLOCK_ORDER);

	printf("  grouping %s: %lu unmovable pages in %lu of %lu pageblocks\n",
	       group ? "on " : "off", unmovable, polluted,
	       PAGES / PAGEBLOCK_NR_PAGES);
	printf("    free 2 MiB blocks: %lu; unusable free index at order %d: %d.%03d\n",
	       huge, PAGEBLOCK_ORDER, unusable_free_index(&z, PAGEBLOCK_ORDER) / 1000,
	       unusable_free_index(&z, PAGEBLOCK_ORDER) % 1000);
	for (int row = 0; row < 4; row++)
		printf("    %.32s\n", map + row * 32);
	printf("    free blocks by order:");
	for (unsigned int o = 0; o < NR_PAGE_ORDERS; o++)
		printf(" %lu", z.free_area[o].nr_free);
	putchar('\n');
	zone_destroy(&z);
}

int main(void)
{
	split_and_merge();
	sizes();
	lock_counts();
	printf("\n4. 256 MiB zone, 75%% full of single pages (10%% unmovable),\n"
	       "   after freeing every movable page\n");
	fragmentation(true);
	fragmentation(false);
	return 0;
}
