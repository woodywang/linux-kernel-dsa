// SPDX-License-Identifier: MIT
/*
 * test_mini_maple.c - randomized tests for mini_maple.c.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -o test_mini_maple test_mini_maple.c
 *   ./test_mini_maple [iterations] [seed]
 *
 * Every operation is mirrored in a plain array (the "model") covering a
 * small index window; after each step the tree must agree with the model,
 * and the tree's structural invariants must hold.
 */
#define MINI_MAPLE_NO_MAIN
#include "mini_maple.c"

#include <assert.h>
#include <string.h>

#define WINDOW 4096UL			/* indices [0, WINDOW) are modelled */

static void *model[WINDOW];
static char vals[64];			/* distinct non-NULL entries */

static int leaf_depth;

/* Check pivots, slot counts, NULL merging, gaps, and equal leaf depth. */
static ul check(const struct node *nd, ul min, ul max, int depth)
{
	assert(nd->n >= 1);
	assert(nd->n <= (nd->leaf ? LEAF_SLOTS : NODE_SLOTS));
	for (int i = 0; i < nd->n - 1; i++) {
		assert(nd->pivot[i] >= min && nd->pivot[i] < max);
		if (i)
			assert(nd->pivot[i] > nd->pivot[i - 1]);
	}
	if (nd->leaf) {
		if (leaf_depth < 0)
			leaf_depth = depth;
		assert(depth == leaf_depth);
		for (int i = 1; i < nd->n; i++)
			assert(nd->slot[i] || nd->slot[i - 1]);
		return node_gap(nd, min, max);
	}
	assert(nd->n >= 2);
	ul best = 0;

	for (int i = 0; i < nd->n; i++) {
		ul g = check(nd->slot[i], slot_min(nd, i, min),
			     slot_max(nd, i, max), depth + 1);
		assert(nd->gap[i] == g);
		if (g > best)
			best = g;
	}
	return best;
}

/* Reference gap search: scan every leaf slot, keep the highest fit. */
static bool brute_gap(const struct node *nd, ul min, ul max,
		      ul lo, ul hi, ul size, ul *start)
{
	bool found = false;

	for (int i = 0; i < nd->n; i++) {
		ul s = slot_min(nd, i, min), e = slot_max(nd, i, max);

		if (!nd->leaf) {
			ul st;

			if (brute_gap(nd->slot[i], s, e, lo, hi, size, &st)) {
				*start = st;
				found = true;
			}
			continue;
		}
		if (nd->slot[i] || e < lo || s > hi)
			continue;
		ul top = e < hi ? e : hi, bottom = s > lo ? s : lo;

		if (top - bottom >= size - 1) {
			*start = top - (size - 1);
			found = true;
		}
	}
	return found;
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 200000;
	unsigned seed = argc > 2 ? atoi(argv[2]) : 1;
	struct mtree t;
	long stores = 0, busy = 0, gaps = 0, max_height = 0;

	srand(seed);
	mt_init(&t);
	for (long it = 0; it < iters; it++) {
		int op = rand() % 10;
		ul a = rand() % WINDOW, b = a + rand() % (rand() % 4 ? 8 : 300);

		if (b >= WINDOW)
			b = WINDOW - 1;
		void *v = rand() % 4 ? &vals[rand() % 64] : NULL;

		if (op < 6) {				/* store a range */
			if (mt_store_range(&t, a, b, v) == -EBUSY) {
				busy++;
			} else {
				for (ul x = a; x <= b; x++)
					model[x] = v;
				stores++;
			}
		} else if (op < 8) {			/* erase */
			ul first, last;
			void *old = mt_load_range(&t, a, &first, &last);
			void *got = mt_erase(&t, a);

			if (got) {
				assert(got == old);
				for (ul x = first; x <= last && x < WINDOW; x++)
					model[x] = NULL;
			}
		} else {				/* gap search */
			ul size = 1 + rand() % 64, s1 = 0, s2 = 0;
			int r = mt_empty_area_rev(&t, a, b + 500, size, &s1);
			bool f = brute_gap(t.root, 0, ULONG_MAX, a, b + 500,
					   size, &s2);

			assert((r == 0) == f);
			if (f) {
				assert(s1 == s2);
				for (ul x = s1; x < s1 + size; x++)
					assert(x >= WINDOW || !model[x]);
				gaps++;
			}
		}

		if (it % 97 == 0 || it == iters - 1) {
			for (ul x = 0; x < WINDOW; x++)
				assert(mt_load(&t, x) == model[x]);
			assert(mt_load(&t, ULONG_MAX) == NULL);
			leaf_depth = -1;
			check(t.root, 0, ULONG_MAX, 1);
			assert(leaf_depth == t.height);
		}
		if (t.height > max_height)
			max_height = t.height;
	}
	printf("ok: %ld iterations, %ld stores (%ld rejected as spanning), "
	       "%ld gaps found, max height %ld, %ld nodes\n",
	       iters, stores, busy, gaps, max_height, t.nodes);
	mt_destroy(&t);
	assert(t.nodes == 0);
	return 0;
}
