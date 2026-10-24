// SPDX-License-Identifier: MIT
/*
 * bench.c - time sort() against the C library's qsort() (Exercise 9.1).
 *
 *   make bench && ./bench [n]
 *
 * Prints, for several input shapes, the comparisons and the time each
 * sort needs.  Times depend on your machine, compiler and C library; the
 * point is to compare shapes, not to quote numbers.  Add your own shapes
 * to make_input(), in particular inputs built to defeat a quicksort's
 * pivot choice.
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sort.h"

static unsigned long ncmp;

static int cmp_int(const void *a, const void *b)
{
	int x = *(const int *)a, y = *(const int *)b;

	ncmp++;
	return (x > y) - (x < y);
}

static const char *const shapes[] = {
	"random", "sorted", "reversed", "all equal", "organ pipe", "few values",
};

static void make_input(int *a, int n, int shape)
{
	for (int i = 0; i < n; i++) {
		switch (shape) {
		case 0: a[i] = rand(); break;
		case 1: a[i] = i; break;
		case 2: a[i] = n - i; break;
		case 3: a[i] = 42; break;
		case 4: a[i] = i < n / 2 ? i : n - i; break;
		default: a[i] = rand() % 4; break;
		}
	}
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 1000000;
	int *in = malloc((size_t)n * sizeof(int));
	int *a = malloc((size_t)n * sizeof(int));

	if (!in || !a)
		return 1;
	printf("%-11s %14s %10s %14s %10s\n", "input", "sort() cmps", "ms",
	       "qsort() cmps", "ms");
	for (int s = 0; s < (int)(sizeof(shapes) / sizeof(shapes[0])); s++) {
		double t0, t1, t2;
		unsigned long c1, c2;

		srand(1);
		make_input(in, n, s);
		memcpy(a, in, (size_t)n * sizeof(int));
		ncmp = 0;
		t0 = now();
		sort(a, (size_t)n, sizeof(int), cmp_int, NULL);
		t1 = now();
		c1 = ncmp;
		memcpy(a, in, (size_t)n * sizeof(int));
		ncmp = 0;
		qsort(a, (size_t)n, sizeof(int), cmp_int);
		t2 = now();
		c2 = ncmp;
		printf("%-11s %14lu %10.1f %14lu %10.1f\n", shapes[s], c1,
		       (t1 - t0) * 1e3, c2, (t2 - t1) * 1e3);
	}
	free(in);
	free(a);
	return 0;
}
