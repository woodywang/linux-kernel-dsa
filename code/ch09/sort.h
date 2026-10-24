// SPDX-License-Identifier: MIT
/* sort.h - a teaching version of the kernel's generic array sort. */
#ifndef SORT_H
#define SORT_H

#include <stddef.h>

/* Same shapes as the kernel's cmp_func_t and swap_func_t. */
typedef int (*cmp_func_t)(const void *a, const void *b);
typedef void (*swap_func_t)(void *a, void *b, int size);

/*
 * Sort @num elements of @size bytes at @base into ascending order.
 * @cmp returns > 0 if a sorts after b.  @swap may be NULL, in which case
 * a built-in swap is chosen from the size and alignment.
 */
void sort(void *base, size_t num, size_t size, cmp_func_t cmp,
	  swap_func_t swap);

/* For experiments: the same heapsort with parts of the tuning turned off. */
enum heap_variant {
	HEAP_KERNEL,		/* bottom-up sift-down, two pops per round */
	HEAP_BOTTOM_UP,		/* bottom-up sift-down, one pop per round */
	HEAP_TOP_DOWN,		/* the common sift-down, one pop per round */
};

void sort_variant(void *base, size_t num, size_t size, cmp_func_t cmp,
		  swap_func_t swap, enum heap_variant v);

#endif /* SORT_H */
