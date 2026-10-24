// SPDX-License-Identifier: MIT
/*
 * sort.c - a teaching version of the kernel's generic array sort
 * (lib/sort.c): an in-place heapsort that never recurses and never
 * allocates, with the kernel's three refinements:
 *
 *   1. Positions are byte offsets, not indexes, so the inner loops never
 *      multiply by the element size.
 *   2. Sift-down is "bottom-up": follow the larger child all the way to a
 *      leaf with one comparison per level, then climb back to where the
 *      sifted element belongs.  Most elements belong near the bottom, so
 *      this needs far fewer comparisons than two per level.
 *   3. The sort phase pops the two largest elements per round.
 *
 * Swaps go through swap.h, which picks a word-sized loop when the element
 * size and alignment allow it.
 *
 * What it leaves out on purpose: sort_r() (a comparison function with a
 * private argument), the _nonatomic() variants that call cond_resched(),
 * and the kernel's branch-free parent() computation.  sort_variant() turns
 * refinements 2 and 3 off for the comparison-count experiments; the kernel
 * has no such switch.
 */
#include "sort.h"
#include "swap.h"

struct sorter {
	char *base;
	size_t size;		/* element size in bytes */
	size_t lsbit;		/* lowest set bit of size */
	cmp_func_t cmp;
	swap_func_t swap;	/* caller's swap, or NULL */
	enum swap_kind kind;	/* built-in swap used when swap is NULL */
};

static int cmp_at(const struct sorter *s, size_t i, size_t j)
{
	return s->cmp(s->base + i, s->base + j);
}

static void swap_at(const struct sorter *s, size_t i, size_t j)
{
	if (s->swap)
		s->swap(s->base + i, s->base + j, (int)s->size);
	else
		swap_builtin(s->kind, s->base + i, s->base + j, s->size);
}

/*
 * Byte offset of the parent of the element at byte offset @i (i > 0).
 * With indexes the parent of j is (j - 1) / 2.  With offsets, i - size is
 * 2 * parent or 2 * parent + size, and one bit of i tells which.
 */
static size_t parent(const struct sorter *s, size_t i)
{
	i -= s->size;
	if (i & s->lsbit)		/* an odd multiple of size */
		i -= s->size;
	return i / 2;
}

/* Sift the element at @a down into the max-heap [0, n). */
static void sift_down(const struct sorter *s, size_t a, size_t n)
{
	size_t i = a, j;

	/* 1. Follow the larger child down to a leaf: one compare per level. */
	for (;;) {
		size_t c = 2 * i + s->size, d = c + s->size;

		if (d < n)
			i = cmp_at(s, c, d) > 0 ? c : d;
		else {
			if (c < n)		/* a last child with no sibling */
				i = c;
			break;
		}
	}
	/* 2. Climb back to the first element on the path that is <= a. */
	while (i != a && cmp_at(s, a, i) >= 0)
		i = parent(s, i);
	/* 3. Put a there; everything above it on the path moves up a level. */
	j = i;
	while (i != a) {
		i = parent(s, i);
		swap_at(s, i, j);
	}
}

/* The common sift-down, for comparison: two compares per level. */
static void sift_down_top_down(const struct sorter *s, size_t a, size_t n)
{
	for (;;) {
		size_t c = 2 * a + s->size, d = c + s->size, big = a;

		if (c < n && cmp_at(s, c, big) > 0)
			big = c;
		if (d < n && cmp_at(s, d, big) > 0)
			big = d;
		if (big == a)
			return;
		swap_at(s, a, big);
		a = big;
	}
}

void sort_variant(void *base, size_t num, size_t size, cmp_func_t cmp,
		  swap_func_t swap, enum heap_variant v)
{
	struct sorter s = {
		.base = base, .size = size, .lsbit = size & -size,
		.cmp = cmp, .swap = swap, .kind = swap_kind_for(base, size),
	};
	void (*sift)(const struct sorter *, size_t, size_t) =
		v == HEAP_TOP_DOWN ? sift_down_top_down : sift_down;
	size_t n = num * size;			/* the heap is [0, n) */
	size_t a = (num / 2) * size;		/* one past the last parent */

	if (num < 2 || size == 0)
		return;

	/* Build a max-heap: sift down every parent, the last one first. */
	while (a) {
		a -= size;
		sift(&s, a, n);
	}

	/* Pop two maxima per round while at least four elements remain. */
	while (v == HEAP_KERNEL && n > 3 * size) {
		size_t second;

		n -= size;
		swap_at(&s, 0, n);		/* the largest to its final slot */
		/* The next largest is still one of the root's children. */
		second = cmp_at(&s, size, 2 * size) > 0 ? size : 2 * size;
		n -= size;
		swap_at(&s, second, n);
		sift(&s, second, n);		/* repair that child's subtree */
		sift(&s, 0, n);			/* then the root */
	}

	/* Pop one maximum per round. */
	while (n > size) {
		n -= size;
		swap_at(&s, 0, n);
		sift(&s, 0, n);
	}
}

void sort(void *base, size_t num, size_t size, cmp_func_t cmp,
	  swap_func_t swap)
{
	sort_variant(base, num, size, cmp, swap, HEAP_KERNEL);
}
