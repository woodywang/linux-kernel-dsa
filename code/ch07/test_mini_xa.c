// SPDX-License-Identifier: MIT
/*
 * test_mini_xa.c - randomized tests for mini_xa.c.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -o test_mini_xa test_mini_xa.c
 *   ./test_mini_xa [iterations] [seed]
 *
 * Every operation is mirrored in a flat model: three windows of 4096
 * indices (at 0, at 2^40, and at the very top of the index space), each
 * index recording its entry, the first index and order of the entry that
 * covers it, and its marks.  After every step the operation's result must
 * agree with the model; periodically the whole tree is compared with the
 * model and its structural invariants are checked.
 */
#define MINI_XA_NO_MAIN
#include "mini_xa.c"

#include <assert.h>
#include <string.h>

#define WIN	4096UL
#define NWIN	3

static const unsigned long base[NWIN] = { 0, 1UL << 40, ~0UL - (WIN - 1) };

static struct {
	void *val;
	unsigned long first;		/* first index of the covering entry */
	unsigned int order;
	unsigned int marks;
} model[NWIN][WIN];

static long vals[64];			/* targets for pointer entries */

static unsigned long seed_state;

static unsigned long rnd(void)
{
	seed_state ^= seed_state << 13;
	seed_state ^= seed_state >> 7;
	seed_state ^= seed_state << 17;
	return seed_state;
}

static void *random_entry(void)
{
	unsigned long r = rnd() % 8;

	if (r == 0)
		return NULL;
	if (r < 4)
		return xa_mk_value(rnd() % 1000);
	return &vals[rnd() % 64];
}

/* The model's version of xa_store_order(). */
static void model_store(int w, unsigned long off, unsigned int order,
			void *entry)
{
	unsigned long first = off, n = 1UL << order;
	unsigned int marks = 0;

	if (model[w][off].val && model[w][off].order >= order) {
		/* Inside an existing entry: it changes as a whole. */
		first = model[w][off].first - base[w];
		n = 1UL << model[w][off].order;
		for (unsigned long i = first; i < first + n; i++) {
			model[w][i].val = entry;
			if (!entry)
				model[w][i].marks = 0;
		}
		return;
	}
	for (unsigned long i = off; i < off + n; i++)
		if (model[w][i].val)
			marks |= model[w][i].marks;
	for (unsigned long i = off; i < off + n; i++) {
		model[w][i].val = entry;
		model[w][i].first = base[w] + first;
		model[w][i].order = order;
		model[w][i].marks = entry ? marks : 0;
	}
}

static void model_mark(int w, unsigned long off, unsigned int mark, bool set)
{
	if (!model[w][off].val)
		return;
	unsigned long first = model[w][off].first - base[w];

	for (unsigned long i = first; i < first + (1UL << model[w][off].order);
	     i++) {
		if (set)
			model[w][i].marks |= 1U << mark;
		else
			model[w][i].marks &= ~(1U << mark);
	}
}

/* First marked entry whose first index is in [start, max]. */
static void *model_find(unsigned long *start, unsigned long max,
			unsigned int mark, unsigned int active)
{
	for (int w = 0; w < NWIN; w++) {
		if (!(active >> w & 1))
			continue;
		for (unsigned long i = 0; i < WIN; i++) {
			unsigned long idx = base[w] + i;

			if (idx < *start || idx > max)
				continue;
			if (model[w][i].val && model[w][i].first == idx &&
			    model[w][i].marks >> mark & 1) {
				*start = idx;
				return model[w][i].val;
			}
		}
	}
	return NULL;
}

static long counted;

static void check_node(const struct xa_node *node, const struct xa_node *parent,
		       unsigned int offset, unsigned int shift)
{
	unsigned int count = 0;

	counted++;
	assert(node->parent == parent);
	assert(node->offset == offset);
	assert(node->shift == shift);
	for (unsigned int i = 0; i < XA_CHUNK_SIZE; i++) {
		void *e = node->slots[i];

		if (!e) {
			for (int m = 0; m < XA_MAX_MARKS; m++)
				assert(!(node->marks[m] >> i & 1));
			continue;
		}
		count++;
		if (xa_is_node(e)) {
			const struct xa_node *child = xa_to_node(e);

			assert(shift > 0);
			check_node(child, node, i, shift - XA_CHUNK_SHIFT);
			for (int m = 0; m < XA_MAX_MARKS; m++)
				assert((node->marks[m] >> i & 1) ==
				       (child->marks[m] != 0));
		} else if (xa_is_sibling(e)) {
			unsigned int c = xa_to_sibling(e);

			assert(c < i);
			assert(node->slots[c] && !xa_is_internal(node->slots[c]));
			for (unsigned int j = c + 1; j < i; j++)
				assert(node->slots[j] == e);
			for (int m = 0; m < XA_MAX_MARKS; m++)
				assert(!(node->marks[m] >> i & 1));
		} else {
			unsigned int n = entry_end(node, i) - i;

			assert(!xa_is_internal(e));
			assert((n & (n - 1)) == 0 && i % n == 0);
		}
	}
	assert(count == node->count && count > 0);
}

static void check_tree(const struct xarray *xa)
{
	counted = 0;
	if (xa_is_node(xa->head)) {
		const struct xa_node *top = xa_to_node(xa->head);

		check_node(top, NULL, 0, top->shift);
		for (int m = 0; m < XA_MAX_MARKS; m++)
			assert((xa->marks >> m & 1) == (top->marks[m] != 0));
		/* Fully shrunk: the top node is not a mere pass-through. */
		assert(!(top->count == 1 && top->slots[0] &&
			 (xa_is_node(top->slots[0]) || !top->shift)));
	} else {
		assert(!xa_is_internal(xa->head));
		if (!xa->head)
			assert(xa->marks == 0);
	}
	assert(counted == xa->nodes);
}

static void check_all(const struct xarray *xa, unsigned int active)
{
	check_tree(xa);
	for (int w = 0; w < NWIN; w++) {
		if (!(active >> w & 1))
			continue;
		for (unsigned long i = 0; i < WIN; i++) {
			unsigned long idx = base[w] + i;

			assert(xa_load(xa, idx) == model[w][i].val);
			for (unsigned int m = 0; m < XA_MAX_MARKS; m++)
				assert(xa_get_mark(xa, idx, m) ==
				       (bool)(model[w][i].marks >> m & 1));
		}
	}
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 200000;
	unsigned long seed = argc > 2 ? strtoul(argv[2], NULL, 0) : 1;
	struct xarray xa = XA_INIT;
	unsigned int active = 1;
	long stores = 0, multi = 0, finds = 0, found = 0, epochs = 0;
	int max_height = 0;

	seed_state = seed * 2654435761UL + 88172645463325252UL;
	for (long it = 0; it < iters; it++) {
		if (it % 5000 == 0) {		/* new epoch: start empty */
			xa_destroy(&xa);
			assert(xa.nodes == 0);
			memset(model, 0, sizeof(model));
			active = 1 + rnd() % 7;	/* which windows are used */
			epochs++;
		}
		int w;

		do
			w = rnd() % NWIN;
		while (!(active >> w & 1));
		unsigned long span = rnd() % 4 ? 128 : WIN;
		unsigned long off = rnd() % span;
		unsigned long op = rnd() % 16;

		if (op < 7) {			/* store, sometimes multi-index */
			unsigned int order = rnd() % 3 ? 0 : rnd() % 13;
			void *entry = random_entry();

			off &= ~((1UL << order) - 1);
			assert(xa_store_order(&xa, base[w] + off, order,
					      entry) == 0);
			model_store(w, off, order, entry);
			stores++;
			multi += order > 0;
		} else if (op < 9) {		/* erase */
			xa_erase(&xa, base[w] + off);
			model_store(w, off, 0, NULL);
		} else if (op < 12) {		/* set or clear a mark */
			unsigned int mark = rnd() % XA_MAX_MARKS;
			bool set = op < 11;

			if (set)
				xa_set_mark(&xa, base[w] + off, mark);
			else
				xa_clear_mark(&xa, base[w] + off, mark);
			model_mark(w, off, mark, set);
		} else if (op < 14) {		/* search for a marked entry */
			unsigned int mark = rnd() % XA_MAX_MARKS;
			unsigned long start = base[w] + off, mstart = start;
			unsigned long max = rnd() % 2 ? ~0UL :
				start + rnd() % WIN;
			void *got = xa_find_marked(&xa, &start, max, mark);
			void *want = model_find(&mstart, max, mark, active);

			assert(got == want);
			if (got)
				assert(start == mstart);
			finds++;
			found += got != NULL;
		} else {			/* load, also outside the windows */
			unsigned long idx = base[w] + off;

			if (op == 15 && w < NWIN - 1)
				idx += WIN + rnd() % 100000;
			void *want = idx - base[w] < WIN ? model[w][off].val : NULL;

			assert(xa_load(&xa, idx) == want);
		}
		if (xa_height(&xa) > max_height)
			max_height = xa_height(&xa);
		if (it % 97 == 0)
			check_all(&xa, active);
	}
	check_all(&xa, active);
	xa_destroy(&xa);
	assert(xa.nodes == 0);
	printf("ok: %ld ops (%ld stores, %ld multi-index), %ld searches "
	       "(%ld hits), %ld epochs, max height %d\n", iters, stores, multi,
	       finds, found, epochs, max_height);
	return 0;
}
