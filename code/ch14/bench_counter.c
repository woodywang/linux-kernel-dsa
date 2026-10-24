// SPDX-License-Identifier: MIT
/*
 * bench_counter.c - one shared atomic counter vs. a sharded counter
 * (Exercise 14.1).  Timing only; the numbers depend on your machine.
 *
 *   cc -O2 -Wall -pthread -o bench_counter bench_counter.c
 *   ./bench_counter [threads] [adds per thread] [batch]
 */
#include <inttypes.h>
#include <stdio.h>
#include <time.h>
#include "pcpu_counter.h"

static _Atomic int64_t shared;
static struct pcpu_counter pc;
static long adds;

static void *add_shared(void *arg)
{
	(void)arg;
	for (long i = 0; i < adds; i++)
		atomic_fetch_add_explicit(&shared, 1, memory_order_relaxed);
	return NULL;
}

static void *add_sharded(void *arg)
{
	int me = (int)(intptr_t)arg;

	for (long i = 0; i < adds; i++)
		pc_add(&pc, me, 1);
	return NULL;
}

static double run(void *(*fn)(void *), int threads)
{
	pthread_t t[256];
	struct timespec a, b;

	clock_gettime(CLOCK_MONOTONIC, &a);
	for (int i = 0; i < threads; i++)
		pthread_create(&t[i], NULL, fn, (void *)(intptr_t)i);
	for (int i = 0; i < threads; i++)
		pthread_join(t[i], NULL);
	clock_gettime(CLOCK_MONOTONIC, &b);
	return (double)(b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

int main(int argc, char **argv)
{
	int threads = argc > 1 ? atoi(argv[1]) : 4;
	int batch = argc > 3 ? atoi(argv[3]) : 32;
	double ts, tp;

	adds = argc > 2 ? atol(argv[2]) : 10000000;
	if (threads < 1 || threads > 256 || batch < 1)
		return 1;
	if (pc_init(&pc, 0, threads, batch))
		return 1;
	ts = run(add_shared, threads);
	tp = run(add_sharded, threads);
	printf("%d threads x %ld adds\n", threads, adds);
	printf("shared atomic:   %.3f s  (total %" PRId64 ")\n", ts,
	       atomic_load(&shared));
	printf("sharded, batch %d: %.3f s  (total %" PRId64 ", read %" PRId64
	       ")\n", batch, tp, pc_sum(&pc), pc_read(&pc));
	pc_destroy(&pc);
	return 0;
}
