// SPDX-License-Identifier: MIT
/*
 * ida_demo.c - how many words does it take to find a free ID?
 *
 * Fills both allocators of idalloc.h, frees a few IDs, and counts the
 * words each search reads.
 *
 *   cc -O2 -Wall -o ida_demo ida_demo.c && ./ida_demo
 */
#include <stdio.h>
#include <stdlib.h>
#include "idalloc.h"

static struct flat_ids flat;
static struct hier_ids hier;

static void alloc_both(const char *what)
{
	long a, b;

	flat.words_read = hier.words_read = 0;
	a = flat_alloc_range(&flat, 0, HB_BITS - 1);
	b = hier_alloc_range(&hier, 0, HB_BITS - 1);
	printf("%s\n", what);
	printf("  flat:         ID %6ld, %4lu words read\n", a, flat.words_read);
	printf("  hierarchical: ID %6ld, %4lu words read\n", b, hier.words_read);
}

int main(void)
{
	hier_init(&hier);
	for (unsigned long i = 0; i < HB_BITS - 1; i++) {
		flat_alloc_range(&flat, 0, HB_BITS - 1);
		hier_alloc_range(&hier, 0, HB_BITS - 1);
	}
	printf("%lu IDs, all but the last in use\n", HB_BITS);
	alloc_both("allocate one more:");

	flat_free(&flat, 70000);
	hier_free(&hier, 70000);
	flat_free(&flat, 200000);
	hier_free(&hier, 200000);
	alloc_both("free 70000 and 200000, allocate one:");
	alloc_both("allocate another:");

	printf("memory: flat %zu bytes, hierarchical %zu bytes\n",
	       sizeof(flat.used), sizeof(hier.store));
	return 0;
}
