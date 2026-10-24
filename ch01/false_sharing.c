// SPDX-License-Identifier: MIT
/*
 * false_sharing.c - two threads, two counters, no shared data.  Only the
 * layout changes: first the counters sit next to each other, then each one
 * gets a cache line of its own.
 *
 *   cc -O2 -Wall -pthread -o false_sharing false_sharing.c && ./false_sharing
 *
 * Timings depend on the machine; run it on a computer with at least two
 * physical cores.
 */
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#define ITERS 200000000UL
#define CACHE_LINE 64

struct packed {			/* both counters in one cache line */
	volatile unsigned long a;
	volatile unsigned long b;
};

struct padded {			/* each counter on its own cache line */
	_Alignas(CACHE_LINE) volatile unsigned long a;
	_Alignas(CACHE_LINE) volatile unsigned long b;
};

static struct packed packed;
static struct padded padded;

static void *bump(void *p)
{
	volatile unsigned long *c = p;

	for (unsigned long i = 0; i < ITERS; i++)
		(*c)++;
	return NULL;
}

static double run(volatile unsigned long *a, volatile unsigned long *b)
{
	struct timespec t0, t1;
	pthread_t x, y;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	pthread_create(&x, NULL, bump, (void *)a);
	pthread_create(&y, NULL, bump, (void *)b);
	pthread_join(x, NULL);
	pthread_join(y, NULL);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	return (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
}

int main(void)
{
	printf("same cache line:      %.2f s\n", run(&packed.a, &packed.b));
	printf("separate cache lines: %.2f s\n", run(&padded.a, &padded.b));
	printf("sizeof(struct packed) = %zu, sizeof(struct padded) = %zu\n",
	       sizeof(struct packed), sizeof(struct padded));
	return 0;
}
