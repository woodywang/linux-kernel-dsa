// SPDX-License-Identifier: MIT
/*
 * stress_counter.c - many threads, one pcpu_counter.
 *
 *   cc -O2 -Wall -fsanitize=thread -pthread -o stress_counter stress_counter.c
 *   ./stress_counter [adds per thread] [seed]
 *
 * Phase 1: each writer thread adds +1 through its own shard and then
 * publishes how many adds it has finished.  A reader thread keeps calling
 * pc_read() and checks that it never goes backwards and is never further
 * below the published progress than the error bound allows.
 *
 * Phase 2: each writer adds random positive and negative amounts whose
 * total it knows in advance, while the reader calls pc_sum() and
 * pc_compare().  At the end of each phase pc_sum() must be exact and
 * pc_read() within the bound.  ThreadSanitizer checks the rest.
 */
#include <inttypes.h>
#include <stdio.h>
#include "pcpu_counter.h"

#define WRITERS	4
#define BATCH	32

struct progress {
	_Atomic long done;
} __attribute__((aligned(CACHELINE)));

static struct pcpu_counter pc;
static struct progress progress[WRITERS];
static _Atomic int phase_over;
static long adds;
static uint64_t seed;

static uint64_t rnd(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

static void fail(const char *what, int64_t a, int64_t b)
{
	fprintf(stderr, "%s: %" PRId64 " vs %" PRId64 "\n", what, a, b);
	exit(1);
}

static void *count_up(void *arg)
{
	int me = (int)(intptr_t)arg;

	for (long i = 1; i <= adds; i++) {
		pc_add(&pc, me, 1);
		atomic_store_explicit(&progress[me].done, i, memory_order_release);
	}
	return NULL;
}

static void *watch_up(void *arg)
{
	int64_t last = 0;
	long reads = 0;

	(void)arg;
	while (!atomic_load(&phase_over)) {
		int64_t lo = 0, r;

		for (int t = 0; t < WRITERS; t++)
			lo += atomic_load_explicit(&progress[t].done,
						   memory_order_acquire);
		r = pc_read(&pc);
		if (r < last)
			fail("pc_read() went backwards", r, last);
		if (r < lo - pc_error_bound(&pc))
			fail("pc_read() too far behind", r, lo);
		last = r;
		reads++;
	}
	return (void *)reads;
}

static int64_t expected_phase2(int me)
{
	uint64_t s = seed + (uint64_t)me;
	int64_t total = 0;

	for (long i = 0; i < adds; i++)
		total += (int64_t)(rnd(&s) % (6 * BATCH + 1)) - 3 * BATCH;
	return total;
}

static void *count_mixed(void *arg)
{
	int me = (int)(intptr_t)arg;
	uint64_t s = seed + (uint64_t)me;

	for (long i = 0; i < adds; i++)
		pc_add(&pc, me, (int64_t)(rnd(&s) % (6 * BATCH + 1)) - 3 * BATCH);
	return NULL;
}

static void *watch_mixed(void *arg)
{
	long reads = 0;

	(void)arg;
	while (!atomic_load(&phase_over)) {
		int64_t s = pc_sum(&pc);

		(void)pc_compare(&pc, s);
		reads++;
	}
	return (void *)reads;
}

static void phase(void *(*writer)(void *), void *(*watcher)(void *),
		  int64_t expect, const char *name)
{
	pthread_t w[WRITERS], r;
	void *reads;

	atomic_store(&phase_over, 0);
	pthread_create(&r, NULL, watcher, NULL);
	for (int t = 0; t < WRITERS; t++)
		pthread_create(&w[t], NULL, writer, (void *)(intptr_t)t);
	for (int t = 0; t < WRITERS; t++)
		pthread_join(w[t], NULL);
	atomic_store(&phase_over, 1);
	pthread_join(r, &reads);
	if (pc_sum(&pc) != expect)
		fail("final pc_sum()", pc_sum(&pc), expect);
	if (abs64(pc_read(&pc) - expect) > pc_error_bound(&pc))
		fail("final pc_read()", pc_read(&pc), expect);
	printf("stress_counter: %s: %d threads x %ld adds, sum exact, "
	       "read within %" PRId64 "\n", name, WRITERS, adds,
	       pc_error_bound(&pc));
}

int main(int argc, char **argv)
{
	int64_t expect;

	adds = argc > 1 ? atol(argv[1]) : 200000;
	seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	seed = seed * 0x9E3779B97F4A7C15ull + 1;
	if (pc_init(&pc, 0, WRITERS, BATCH))
		return 1;
	expect = (int64_t)WRITERS * adds;
	phase(count_up, watch_up, expect, "+1 adds");
	for (int t = 0; t < WRITERS; t++)
		expect += expected_phase2(t);
	phase(count_mixed, watch_mixed, expect, "mixed adds");
	pc_destroy(&pc);
	return 0;
}
