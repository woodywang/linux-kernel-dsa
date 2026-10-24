// SPDX-License-Identifier: MIT
/*
 * mini_maple.c - a small teaching version of the maple tree's core ideas.
 *
 * A B-tree that maps every index in [0, ULONG_MAX] to a pointer.  Runs of
 * indices that share a value are stored as one range; holes are stored as
 * NULL ranges.  Leaves hold up to 16 ranges, internal nodes up to 10 children
 * plus, for each child, the size of the largest NULL range below it.
 *
 * What it shows:              lookup, range store (one store can add or
 *                             remove several slots), upward splits, gap
 *                             tracking, and top-down gap search.
 * What it leaves out on purpose: RCU, rebalancing after erase, pushing data
 *                             into siblings, and stores that cross a leaf
 *                             boundary (the kernel calls these "spanning
 *                             stores"; here they return -EBUSY).
 *
 * Build and run the demo:     cc -O2 -Wall -o mini_maple mini_maple.c
 *                             ./mini_maple
 */
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define LEAF_SLOTS	16	/* ranges per leaf */
#define NODE_SLOTS	10	/* children per internal node */

typedef unsigned long ul;

struct node {
	bool leaf;
	int n;				/* slots in use, at least 1 */
	ul pivot[LEAF_SLOTS - 1];	/* pivot[i] = last index of slot i */
	void *slot[LEAF_SLOTS];		/* leaf: entries; internal: children */
	ul gap[NODE_SLOTS];		/* internal: largest NULL range in child i */
};

struct mtree {
	struct node *root;
	int height;
	long nodes;			/* nodes currently allocated */
};

/* The range covered by slot i of a node that covers [min, max]. */
static ul slot_min(const struct node *nd, int i, ul min)
{
	return i ? nd->pivot[i - 1] + 1 : min;
}

static ul slot_max(const struct node *nd, int i, ul max)
{
	return i < nd->n - 1 ? nd->pivot[i] : max;
}

/* Number of indices in [first, last], saturating at ULONG_MAX. */
static ul range_size(ul first, ul last)
{
	return last - first == ULONG_MAX ? ULONG_MAX : last - first + 1;
}

static struct node *node_new(struct mtree *t, bool leaf)
{
	struct node *nd = calloc(1, sizeof(*nd));

	if (!nd)
		abort();
	nd->leaf = leaf;
	t->nodes++;
	return nd;
}

void mt_init(struct mtree *t)
{
	t->nodes = 0;
	t->root = node_new(t, true);	/* one NULL range: [0, ULONG_MAX] */
	t->root->n = 1;
	t->height = 1;
}

/* ---- Lookup ----------------------------------------------------------- */

void *mt_load(const struct mtree *t, ul index)
{
	const struct node *nd = t->root;

	for (;;) {
		int i = 0;

		/* Linear scan: stop at the first pivot >= index. */
		while (i < nd->n - 1 && nd->pivot[i] < index)
			i++;
		if (nd->leaf)
			return nd->slot[i];
		nd = nd->slot[i];
	}
}

/* Like mt_load(), but also report the range the entry covers. */
void *mt_load_range(const struct mtree *t, ul index, ul *first, ul *last)
{
	const struct node *nd = t->root;
	ul min = 0, max = ULONG_MAX;

	for (;;) {
		int i = 0;

		while (i < nd->n - 1 && nd->pivot[i] < index)
			i++;
		ul lo = slot_min(nd, i, min), hi = slot_max(nd, i, max);

		if (nd->leaf) {
			*first = lo;
			*last = hi;
			return nd->slot[i];
		}
		min = lo;
		max = hi;
		nd = nd->slot[i];
	}
}

/* ---- Gap bookkeeping ---------------------------------------------------- */

/* Largest NULL range anywhere below a node that covers [min, max]. */
static ul node_gap(const struct node *nd, ul min, ul max)
{
	ul best = 0;

	for (int i = 0; i < nd->n; i++) {
		ul g;

		if (nd->leaf)
			g = nd->slot[i] ? 0 :
			    range_size(slot_min(nd, i, min), slot_max(nd, i, max));
		else
			g = nd->gap[i];
		if (g > best)
			best = g;
	}
	return best;
}

/* ---- Store -------------------------------------------------------------- */

struct range { ul first, last; void *entry; };	/* one leaf slot */
struct kid { struct node *child; ul last; };	/* one internal slot */

/* If a node had to split, its new right half and the left half's last index. */
struct split { struct node *right; ul pivot; };

static void leaf_fill(struct node *nd, const struct range *r, int cnt)
{
	nd->n = cnt;
	for (int i = 0; i < cnt; i++) {
		nd->slot[i] = r[i].entry;
		if (i < cnt - 1)
			nd->pivot[i] = r[i].last;
	}
}

static void internal_fill(struct node *nd, const struct kid *k, int cnt, ul min)
{
	nd->n = cnt;
	for (int i = 0; i < cnt; i++) {
		nd->slot[i] = k[i].child;
		if (i < cnt - 1)
			nd->pivot[i] = k[i].last;
		nd->gap[i] = node_gap(k[i].child, min, k[i].last);
		min = k[i].last + 1;	/* wraps only after the last child */
	}
}

/*
 * Store @entry over [first, last] in a leaf that covers [min, max].
 * The leaf is rebuilt in a buffer two ranges larger than a node: a store
 * can split one range into three (head, new, tail).  If the result does
 * not fit, the leaf splits and the caller must add the right half.
 */
static void leaf_store(struct mtree *t, struct node *nd, ul min, ul max,
		       ul first, ul last, void *entry, struct split *sp)
{
	struct range buf[LEAF_SLOTS + 2];
	int cnt = 0, m = 0;
	bool placed = false;

	for (int i = 0; i < nd->n; i++) {
		ul s = slot_min(nd, i, min), e = slot_max(nd, i, max);
		void *v = nd->slot[i];

		if (e < first || s > last) {		/* untouched */
			buf[cnt++] = (struct range){ s, e, v };
			continue;
		}
		if (s < first)				/* keep the head */
			buf[cnt++] = (struct range){ s, first - 1, v };
		if (!placed) {				/* the new range */
			buf[cnt++] = (struct range){ first, last, entry };
			placed = true;
		}
		if (e > last)				/* keep the tail */
			buf[cnt++] = (struct range){ last + 1, e, v };
	}

	/* Merge neighbouring NULL ranges so each hole is one slot. */
	for (int i = 0; i < cnt; i++) {
		if (m && !buf[i].entry && !buf[m - 1].entry)
			buf[m - 1].last = buf[i].last;
		else
			buf[m++] = buf[i];
	}

	sp->right = NULL;
	if (m <= LEAF_SLOTS) {
		leaf_fill(nd, buf, m);
		return;
	}
	/* Too many ranges: split in half and hand the right half upward. */
	int k = m / 2;

	sp->right = node_new(t, true);
	sp->pivot = buf[k - 1].last;
	leaf_fill(nd, buf, k);
	leaf_fill(sp->right, buf + k, m - k);
}

static int node_store(struct mtree *t, struct node *nd, ul min, ul max,
		      ul first, ul last, void *entry, struct split *sp)
{
	if (nd->leaf) {
		if (last > max)
			return -EBUSY;		/* crosses a leaf boundary */
		leaf_store(t, nd, min, max, first, last, entry, sp);
		return 0;
	}

	/* Descend into the child that contains @first. */
	int i = 0;

	while (i < nd->n - 1 && nd->pivot[i] < first)
		i++;
	ul cmin = slot_min(nd, i, min), cmax = slot_max(nd, i, max);
	struct split child;
	int ret = node_store(t, nd->slot[i], cmin, cmax, first, last, entry,
			     &child);

	sp->right = NULL;
	if (ret)
		return ret;
	if (!child.right) {			/* no split below: fix one gap */
		nd->gap[i] = node_gap(nd->slot[i], cmin, cmax);
		return 0;
	}

	/* The child split: insert its right half just after it. */
	struct kid buf[NODE_SLOTS + 1];
	int cnt = 0;

	for (int j = 0; j < nd->n; j++) {
		buf[cnt++] = (struct kid){ nd->slot[j], slot_max(nd, j, max) };
		if (j == i) {
			buf[cnt - 1].last = child.pivot;
			buf[cnt++] = (struct kid){ child.right, cmax };
		}
	}
	if (cnt <= NODE_SLOTS) {
		internal_fill(nd, buf, cnt, min);
		return 0;
	}
	/* This node overflowed too: split it and pass the problem up. */
	int k = cnt / 2;

	sp->right = node_new(t, false);
	sp->pivot = buf[k - 1].last;
	internal_fill(nd, buf, k, min);
	internal_fill(sp->right, buf + k, cnt - k, sp->pivot + 1);
	return 0;
}

/* Store @entry (possibly NULL) over [first, last]. */
int mt_store_range(struct mtree *t, ul first, ul last, void *entry)
{
	struct split sp;
	int ret;

	if (first > last)
		return -EINVAL;
	ret = node_store(t, t->root, 0, ULONG_MAX, first, last, entry, &sp);
	if (ret || !sp.right)
		return ret;

	/* The root split: grow the tree by one level. */
	struct node *root = node_new(t, false);
	struct kid kids[2] = {
		{ t->root, sp.pivot },
		{ sp.right, ULONG_MAX },
	};

	internal_fill(root, kids, 2, 0);
	t->root = root;
	t->height++;
	return 0;
}

/* Store only if all of [first, last] is currently NULL. */
int mt_insert_range(struct mtree *t, ul first, ul last, void *entry)
{
	ul idx = first, lo, hi;

	if (first > last || !entry)
		return -EINVAL;
	for (;;) {
		if (mt_load_range(t, idx, &lo, &hi))
			return -EEXIST;
		if (hi >= last)
			break;
		idx = hi + 1;
	}
	return mt_store_range(t, first, last, entry);
}

/* Remove the whole range that contains @index; return its entry. */
void *mt_erase(struct mtree *t, ul index)
{
	ul first, last;
	void *old = mt_load_range(t, index, &first, &last);

	if (old && mt_store_range(t, first, last, NULL))
		return NULL;
	return old;
}

/* ---- Gap search --------------------------------------------------------- */

/*
 * Find the highest run of @size NULL indices inside [lo, hi], searching a
 * node that covers [min, max].  Internal nodes skip any child whose gap is
 * too small, so the search touches only a few paths of the tree.
 */
static bool gap_rev(const struct node *nd, ul min, ul max,
		    ul lo, ul hi, ul size, ul *start)
{
	for (int i = nd->n - 1; i >= 0; i--) {
		ul s = slot_min(nd, i, min), e = slot_max(nd, i, max);

		if (s > hi)
			continue;		/* above the window */
		if (e < lo)
			break;			/* below it: nothing left to try */
		if (nd->leaf) {
			ul top = e < hi ? e : hi, bottom = s > lo ? s : lo;

			if (!nd->slot[i] && top - bottom >= size - 1) {
				*start = top - (size - 1);
				return true;
			}
		} else if (nd->gap[i] >= size &&
			   gap_rev(nd->slot[i], s, e, lo, hi, size, start)) {
			return true;
		}
	}
	return false;
}

int mt_empty_area_rev(const struct mtree *t, ul lo, ul hi, ul size, ul *start)
{
	if (!size || lo > hi || hi - lo < size - 1)
		return -EINVAL;
	return gap_rev(t->root, 0, ULONG_MAX, lo, hi, size, start) ? 0 : -EBUSY;
}

/* ---- Teardown ----------------------------------------------------------- */

static void node_free(struct mtree *t, struct node *nd)
{
	if (!nd->leaf)
		for (int i = 0; i < nd->n; i++)
			node_free(t, nd->slot[i]);
	free(nd);
	t->nodes--;
}

void mt_destroy(struct mtree *t)
{
	node_free(t, t->root);
	t->root = NULL;
}

/* ---- Demo: a toy address space ------------------------------------------ */
#ifndef MINI_MAPLE_NO_MAIN

#define PAGE	0x1000UL
#define LOW	0x10000UL
#define HIGH	0x7fffffff0000UL

static ul toy_mmap(struct mtree *t, ul len, const char *name)
{
	ul start;

	if (mt_empty_area_rev(t, LOW, HIGH - 1, len, &start))
		return 0;
	if (mt_store_range(t, start, start + len - 1, (void *)name))
		return 0;
	return start;
}

int main(void)
{
	struct mtree t;
	ul stack, libc, heap, a, first, last;
	int placed = 0;

	mt_init(&t);
	stack = toy_mmap(&t, 0x800000, "stack");
	libc = toy_mmap(&t, 0x200000, "libc");
	heap = toy_mmap(&t, 0x10000, "heap");
	printf("stack %#lx, libc %#lx, heap %#lx\n", stack, libc, heap);

	printf("insert over libc: %d (expect %d)\n",
	       mt_insert_range(&t, libc, libc + PAGE - 1, "clash"), -EEXIST);
	printf("insert at LOW:    %d (expect 0)\n",
	       mt_insert_range(&t, LOW, LOW + 16 * PAGE - 1, "text"));

	mt_erase(&t, libc + 5);			/* any index inside works */
	a = toy_mmap(&t, PAGE, "anon");
	printf("anon placed right below stack? %s\n",
	       a + PAGE == stack ? "yes" : "no");

	printf("lookup LOW+3 pages -> %s\n",
	       (char *)mt_load(&t, LOW + 3 * PAGE));
	mt_load_range(&t, LOW + 64 * PAGE, &first, &last);
	printf("hole above text: [%#lx, %#lx]\n", first, last);

	for (int i = 0; i < 2000; i++)
		placed += toy_mmap(&t, PAGE * (1 + i % 7), "small") != 0;
	printf("placed %d more: height %d, %ld nodes\n",
	       placed, t.height, t.nodes);

	mt_destroy(&t);
	return 0;
}
#endif
