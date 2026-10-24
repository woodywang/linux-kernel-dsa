// SPDX-License-Identifier: MIT
/*
 * test_min_heap.c - randomized tests for min_heap.h.
 *
 *   cc -O2 -Wall -Wextra -fsanitize=address,undefined \
 *      -o test_min_heap test_min_heap.c
 *   ./test_min_heap [steps] [seed]
 *
 * Two heaps are exercised side by side: one of ints with the built-in
 * swap, and one of 12-byte records with a caller-supplied swap.  Every
 * operation (push, pop, pop_push, delete at a random index, heapify after
 * scribbling over the array) is mirrored in a model that is simply a
 * multiset kept as a sorted array.  After each step the heap property is
 * checked for every parent-child pair, the minimum is compared with the
 * model's, and every 64 steps the whole contents are compared.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "min_heap.h"

#define CAP 300

struct rec {
	int key;
	int a, b;		/* payload that must travel with the key */
};

DEFINE_MIN_HEAP(int, int_heap);
DEFINE_MIN_HEAP(struct rec, rec_heap);

static bool int_less(const void *l, const void *r, void *args)
{
	(void)args;
	return *(const int *)l < *(const int *)r;
}

static bool rec_less(const void *l, const void *r, void *args)
{
	(void)args;
	return ((const struct rec *)l)->key < ((const struct rec *)r)->key;
}

static void rec_swap(void *l, void *r, void *args)
{
	struct rec t = *(struct rec *)l;

	++*(unsigned long *)args;
	*(struct rec *)l = *(struct rec *)r;
	*(struct rec *)r = t;
}

static const struct min_heap_callbacks int_cb = { .less = int_less };
static const struct min_heap_callbacks rec_cb = { .less = rec_less,
						  .swp = rec_swap };

static int model[CAP], mlen;

static int icmp(const void *a, const void *b)
{
	return (*(const int *)a > *(const int *)b) -
	       (*(const int *)a < *(const int *)b);
}

static void m_add(int k)
{
	int i = mlen++;

	while (i > 0 && model[i - 1] > k) {
		model[i] = model[i - 1];
		i--;
	}
	model[i] = k;
}

static void m_remove(int k)
{
	for (int i = 0; i < mlen; i++)
		if (model[i] == k) {
			memmove(model + i, model + i + 1,
				(size_t)(mlen - i - 1) * sizeof(int));
			mlen--;
			return;
		}
	assert(!"key not in model");
}

static struct rec mkrec(int k)
{
	return (struct rec){ k, k * 3 + 1, -k };
}

static void check(const struct int_heap *h, const struct rec_heap *g,
		  bool full)
{
	assert((int)h->nr == mlen && (int)g->nr == mlen);
	for (size_t i = 1; i < h->nr; i++) {
		assert(h->data[(i - 1) / 2] <= h->data[i]);
		assert(g->data[(i - 1) / 2].key <= g->data[i].key);
	}
	for (size_t i = 0; i < g->nr; i++)	/* payload stayed attached */
		assert(g->data[i].a == g->data[i].key * 3 + 1 &&
		       g->data[i].b == -g->data[i].key);
	if (mlen) {
		assert(*min_heap_peek(h) == model[0]);
		assert(min_heap_peek(g)->key == model[0]);
	} else {
		assert(!min_heap_peek(h) && !min_heap_peek(g));
	}
	if (full) {
		int tmp[CAP];

		memcpy(tmp, h->data, h->nr * sizeof(int));
		qsort(tmp, h->nr, sizeof(int), icmp);
		assert(memcmp(tmp, model, h->nr * sizeof(int)) == 0);
		for (size_t i = 0; i < g->nr; i++)
			tmp[i] = g->data[i].key;
		qsort(tmp, g->nr, sizeof(int), icmp);
		assert(memcmp(tmp, model, g->nr * sizeof(int)) == 0);
	}
}

int main(int argc, char **argv)
{
	long steps = argc > 1 ? atol(argv[1]) : 200000;
	unsigned seed = argc > 2 ? (unsigned)atoi(argv[2]) : 1;
	static int ibuf[CAP];
	static struct rec rbuf[CAP];
	struct int_heap h;
	struct rec_heap g;
	unsigned long swaps = 0;
	int range = 50;

	srand(seed);
	min_heap_init(&h, ibuf, CAP);
	min_heap_init(&g, rbuf, CAP);
	for (long s = 0; s < steps; s++) {
		int op = rand() % 100, k = rand() % range;

		if (s % 20000 == 0)
			range = range == 50 ? 1000000 : 50;	/* ties, then not */
		if (op < 45) {				/* push */
			struct rec rk = mkrec(k);
			bool ok1 = min_heap_push(&h, &k, &int_cb, NULL);
			bool ok2 = min_heap_push(&g, &rk, &rec_cb, &swaps);

			assert(ok1 == ok2 && ok1 == (mlen < CAP));
			if (ok1)
				m_add(k);
		} else if (op < 75) {			/* pop */
			bool ok1 = min_heap_pop(&h, &int_cb, NULL);
			bool ok2 = min_heap_pop(&g, &rec_cb, &swaps);

			assert(ok1 == ok2 && ok1 == (mlen > 0));
			if (ok1)
				m_remove(model[0]);
		} else if (op < 85 && mlen) {		/* replace the minimum */
			struct rec rk = mkrec(k);

			m_remove(model[0]);
			min_heap_pop_push(&h, &k, &int_cb, NULL);
			min_heap_pop_push(&g, &rk, &rec_cb, &swaps);
			m_add(k);
		} else if (op < 97 && mlen) {		/* delete anywhere */
			size_t idx = (size_t)rand() % h.nr;
			int victim = h.data[idx];

			/* Delete the same key from g, wherever it sits. */
			size_t j = 0;

			while (g.data[j].key != victim)
				j++;
			assert(min_heap_del(&h, idx, &int_cb, NULL));
			assert(min_heap_del(&g, j, &rec_cb, &swaps));
			m_remove(victim);
		} else {				/* rebuild from scratch */
			for (size_t i = 0; i < h.nr; i++) {
				int r = rand() % range;

				m_remove(h.data[i]);
				h.data[i] = r;
				g.data[i] = mkrec(r);
				m_add(r);
			}
			min_heapify_all(&h, &int_cb, NULL);
			min_heapify_all(&g, &rec_cb, &swaps);
		}
		check(&h, &g, s % 64 == 0);
	}
	assert(!min_heap_del(&h, h.nr, &int_cb, NULL));	/* out of range */
	printf("test_min_heap: %ld steps, %lu custom swaps, seed %u\n",
	       steps, swaps, seed);
	return 0;
}
