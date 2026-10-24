// SPDX-License-Identifier: MIT
/*
 * sort_demo.c - count the comparisons (and swaps) each sort makes.
 *
 * Distinct keys in random order, then all-equal keys.  The counts are
 * deterministic (a fixed generator, not rand()), so the chapter can quote
 * them.  Timing is left to bench.c.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "list_sort.h"
#include "min_heap.h"
#include "sort.h"

static unsigned long ncmp, nswap;
static unsigned long long rng = 88172645463325252ULL;

static unsigned long long next_rand(void)	/* xorshift64 */
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

static int cmp_int(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;

	ncmp++;
	return (x > y) - (x < y);
}

static void swap_int(void *a, void *b, int size)
{
	int t = *(int *)a;

	(void)size;
	nswap++;
	*(int *)a = *(int *)b;
	*(int *)b = t;
}

struct item {
	int key;
	struct list_head node;
};

static int cmp_item(void *priv, const struct list_head *a,
		    const struct list_head *b)
{
	(void)priv;
	ncmp++;
	return list_entry(a, struct item, node)->key >
	       list_entry(b, struct item, node)->key;
}

static bool less_int(const void *a, const void *b, void *args)
{
	(void)args;
	ncmp++;
	return *(const int *)a < *(const int *)b;
}

static void swp_int(void *a, void *b, void *args)
{
	(void)args;
	swap_int(a, b, sizeof(int));
}

static const struct min_heap_callbacks heap_cb = { less_int, swp_int };
DEFINE_MIN_HEAP(int, int_heap);

static void make_keys(int *keys, int n, bool equal)
{
	for (int i = 0; i < n; i++)
		keys[i] = equal ? 7 : i;
	for (int i = n - 1; i > 0 && !equal; i--) {	/* shuffle */
		int j = (int)(next_rand() % (unsigned)(i + 1)), t = keys[i];

		keys[i] = keys[j];
		keys[j] = t;
	}
}

static unsigned long heap_cmp(const int *keys, int n, enum heap_variant v,
			      unsigned long *swaps)
{
	int *a = malloc((size_t)n * sizeof(int));

	for (int i = 0; i < n; i++)
		a[i] = keys[i];
	ncmp = nswap = 0;
	sort_variant(a, (size_t)n, sizeof(int), cmp_int, swap_int, v);
	free(a);
	*swaps = nswap;
	return ncmp;
}

static unsigned long list_cmp(const int *keys, int n)
{
	struct item *items = malloc((size_t)n * sizeof(*items));
	LIST_HEAD(head);

	for (int i = 0; i < n; i++) {
		items[i].key = keys[i];
		list_add_tail(&items[i].node, &head);
	}
	ncmp = 0;
	list_sort(NULL, &head, cmp_item);
	free(items);
	return ncmp;
}

/* Fill a min-heap with the keys, then pop them all. */
static unsigned long heap_pops(const int *keys, int n, unsigned long *swaps)
{
	int *buf = malloc((size_t)n * sizeof(int));
	struct int_heap h;

	min_heap_init(&h, buf, (size_t)n);
	for (int i = 0; i < n; i++)
		buf[i] = keys[i];
	h.nr = (size_t)n;
	min_heapify_all(&h, &heap_cb, NULL);
	ncmp = nswap = 0;
	while (h.nr)
		min_heap_pop(&h, &heap_cb, NULL);
	free(buf);
	*swaps = nswap;
	return ncmp;
}

int main(void)
{
	static const int sizes[] = { 1000, 10000, 100000 };
	unsigned long s1, s2, s3;

	printf("Comparisons, distinct keys in random order\n");
	printf("%8s %10s %10s %10s %10s %10s\n", "n", "log2(n!)",
	       "top-down", "bottom-up", "two pops", "list_sort");
	for (int k = 0; k < 3; k++) {
		int n = sizes[k], *keys = malloc((size_t)n * sizeof(int));

		make_keys(keys, n, false);
		printf("%8d %10.0f %10lu %10lu %10lu %10lu\n", n,
		       lgamma(n + 1.0) / log(2.0),
		       heap_cmp(keys, n, HEAP_TOP_DOWN, &s1),
		       heap_cmp(keys, n, HEAP_BOTTOM_UP, &s2),
		       heap_cmp(keys, n, HEAP_KERNEL, &s3),
		       list_cmp(keys, n));
		if (n == 100000)
			printf("%8s %10s %10lu %10lu %10lu   (swaps)\n", "", "",
			       s1, s2, s3);
		free(keys);
	}

	int n = 10000, *keys = malloc((size_t)n * sizeof(int));
	unsigned long c1, c2, c3;

	make_keys(keys, n, true);
	c1 = heap_cmp(keys, n, HEAP_TOP_DOWN, &s1);
	c2 = heap_cmp(keys, n, HEAP_BOTTOM_UP, &s2);
	c3 = heap_cmp(keys, n, HEAP_KERNEL, &s3);
	printf("\nComparisons, %d equal keys\n", n);
	printf("sort: top-down %lu, bottom-up %lu, two pops %lu; list_sort %lu\n",
	       c1, c2, c3, list_cmp(keys, n));
	c1 = heap_pops(keys, n, &s1);
	make_keys(keys, n, false);
	c2 = heap_pops(keys, n, &s2);
	printf("min_heap, pop all: equal keys %lu compares, %lu swaps; "
	       "distinct keys %lu compares, %lu swaps\n", c1, s1, c2, s2);
	free(keys);
	return 0;
}
