// SPDX-License-Identifier: MIT
/*
 * traces.h - synthetic access traces for the reclaim simulator.
 *
 * All traces are generated with integer arithmetic from a fixed seed, so
 * every compiler and machine produces the same trace and the same results.
 */
#ifndef TRACES_H
#define TRACES_H

#include "reclaim.h"

static uint64_t rng_state;

static inline void rng_seed(uint64_t seed)
{
	rng_state = seed * 0x9E3779B97F4A7C15ull + 1;
}

static inline uint32_t rng(void)		/* xorshift64* */
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return (rng_state * 0x2545F4914F6CDD1Dull) >> 32;
}

/* A file read in a loop that is 25% larger than memory. */
static inline long trace_loop(uint32_t *t, long n, int cap)
{
	int len = cap + cap / 4;

	for (long i = 0; i < n; i++)
		t[i] = (i % len) | FD_ACCESS;
	return n;
}

/*
 * A hot set of mapped pages (60% of memory), used at random, while a
 * large file is streamed through once with read().
 */
static inline long trace_scan(uint32_t *t, long n, int cap)
{
	int hot = cap * 6 / 10;
	uint32_t next_stream = hot;

	for (long i = 0; i < n; i++) {
		if (rng() % 2)
			t[i] = rng() % hot;
		else
			t[i] = (next_stream++ % (MAX_PAGES - hot) + hot) | FD_ACCESS;
	}
	return n;
}

/* Skewed random mapped accesses over four times memory: the minimum of
 * three uniform draws favours low page numbers. */
static inline long trace_skew(uint32_t *t, long n, int cap)
{
	uint32_t span = 4 * cap;

	for (long i = 0; i < n; i++) {
		uint32_t a = rng() % span, b = rng() % span, c = rng() % span;
		uint32_t m = a < b ? a : b;

		t[i] = m < c ? m : c;
	}
	return n;
}

/*
 * The working set changes: for the first half, a set of pages covering
 * 75% of memory is read over and over in order; for the second half, a
 * different set of the same size.
 */
static inline long trace_shift(uint32_t *t, long n, int cap)
{
	int ws = cap * 3 / 4;

	for (long i = 0; i < n; i++) {
		uint32_t base = i < n / 2 ? 0 : 2 * cap;

		t[i] = (base + i % ws) | FD_ACCESS;
	}
	return n;
}

#endif
