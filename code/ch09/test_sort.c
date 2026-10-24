// SPDX-License-Identifier: MIT
/*
 * test_sort.c - randomized tests for sort.c, checked against qsort().
 *
 *   cc -O2 -Wall -Wextra -fsanitize=address,undefined \
 *      -o test_sort test_sort.c sort.c
 *   ./test_sort [rounds] [seed]
 *
 * Each round sorts an array of random length with a random element size
 * (1 to 24 bytes, so that all three built-in swaps are used), at an
 * aligned or a deliberately misaligned base address, filled with one of
 * several patterns: random, few distinct values, sorted, reversed, all
 * equal, organ pipe.  Elements are compared as whole byte strings, which
 * is a total order, so the result must equal qsort()'s byte for byte, for
 * all three variants and with a caller-supplied swap function.  The test
 * also checks the comparison count against the bound in the kernel's
 * comment: 1.5 n log2 n + O(n) in the worst case.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sort.h"

static size_t esize;		/* element size for the comparison */
static unsigned long ncmp, nswap;

static int cmp_bytes(const void *a, const void *b)
{
	ncmp++;
	return memcmp(a, b, esize);
}

static int qsort_cmp(const void *a, const void *b)
{
	return memcmp(a, b, esize);
}

/* A caller-supplied swap, as a user with extra bookkeeping would pass. */
static void swap_counting(void *a, void *b, int size)
{
	unsigned char *x = a, *y = b;

	nswap++;
	for (int i = 0; i < size; i++) {
		unsigned char t = x[i];

		x[i] = y[i];
		y[i] = t;
	}
}

static void fill(unsigned char *p, size_t num, size_t size, int pattern)
{
	size_t klen = size < 8 ? size : 8;	/* bytes holding the key */

	for (size_t i = 0; i < num; i++) {
		unsigned char *e = p + i * size;
		unsigned long long key;

		switch (pattern) {
		case 0: key = (unsigned long long)rand() << 20 ^ rand(); break;
		case 1: key = rand() % 4; break;		/* few values */
		case 2: key = i; break;				/* sorted */
		case 3: key = num - i; break;			/* reversed */
		case 4: key = 7; break;				/* all equal */
		default: key = i < num / 2 ? i : num - i; break; /* organ pipe */
		}
		/* The key big-endian first, so memcmp() orders by key. */
		for (size_t b = 0; b < size; b++)
			e[b] = b < klen ? (unsigned char)(key >> 8 * (klen - 1 - b))
					: (unsigned char)(pattern == 4 ? 0 : rand());
	}
}

int main(int argc, char **argv)
{
	long rounds = argc > 1 ? atol(argv[1]) : 3000;
	unsigned seed = argc > 2 ? (unsigned)atoi(argv[2]) : 1;
	static const size_t sizes[] = { 1, 2, 3, 4, 5, 8, 12, 16, 24 };
	unsigned long checked = 0;

	srand(seed);
	for (long r = 0; r < rounds; r++) {
		size_t num = (size_t)(rand() % 3) == 0 ? (size_t)rand() % 8 :
			     (size_t)rand() % 2000;
		int pattern = rand() % 6;
		size_t off = (size_t)rand() % 2;	/* misalign the base */
		unsigned char *buf, *in, *ref, *a;

		esize = sizes[rand() % (int)(sizeof(sizes) / sizeof(sizes[0]))];
		buf = malloc(num * esize + 8);
		in = malloc(num * esize + 1);
		ref = malloc(num * esize + 1);
		assert(buf && in && ref);
		a = buf + off;
		fill(in, num, esize, pattern);
		memcpy(ref, in, num * esize);
		qsort(ref, num, esize, qsort_cmp);

		/* Three variants with built-in swaps, then a custom swap. */
		for (int v = 0; v <= HEAP_TOP_DOWN + 1; v++) {
			memcpy(a, in, num * esize);
			ncmp = nswap = 0;
			if (v <= HEAP_TOP_DOWN)
				sort_variant(a, num, esize, cmp_bytes, NULL,
					     (enum heap_variant)v);
			else
				sort(a, num, esize, cmp_bytes, swap_counting);
			if (num && memcmp(a, ref, num * esize) != 0) {
				printf("FAIL: round %ld variant %d n=%zu size=%zu "
				       "pattern %d\n", r, v, num, esize, pattern);
				return 1;
			}
			if (v == HEAP_KERNEL && num > 1) {
				double bound = 1.5 * num * log2((double)num) +
					       4.0 * num;
				assert(ncmp <= bound);
			}
			if (v > HEAP_TOP_DOWN && num > 1)
				assert(nswap > 0);	/* the custom swap ran */
			checked++;
		}
		free(in);
		free(buf);
		free(ref);
	}
	printf("test_sort: %lu sorts checked against qsort(), seed %u\n",
	       checked, seed);
	return 0;
}
