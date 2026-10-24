// SPDX-License-Identifier: MIT
/*
 * test_bloom.c - check bloom.h and measure its false-positive rate.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -o test_bloom test_bloom.c -lm
 *   ./test_bloom
 *
 * The false-positive test uses pseudo-random 8-byte-aligned addresses
 * (Exercise 17.5 asks what happens with consecutive ones); the
 * double-buffering demo uses consecutive PMD entries, as a page-table
 * walk sees them.  The output is deterministic; Section 17.5 quotes it.
 */
#include "bloom.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

#define BASE	0xffff888012340000ull	/* a made-up page-table address */

static uintptr_t pmd(long i)
{
	return BASE + 8 * i;
}

static uint64_t x = 88172645463325252ull;

static uintptr_t random_pmd(void)		/* xorshift64 */
{
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return BASE + 8 * (x % (1ull << 36));
}

static void false_positives(long n)
{
	struct bloom_pair b;
	long fp = 0, probes = 100000;
	static uintptr_t items[20000];

	assert(bloom_init(&b) && n <= 20000);
	for (long i = 0; i < n; i++) {
		items[i] = random_pmd();
		bloom_add(&b, 1, items[i]);
	}
	for (long i = 0; i < n; i++)
		assert(bloom_test(&b, 1, items[i]));	/* no false negatives */
	for (long i = 0; i < probes; i++)
		fp += bloom_test(&b, 1, random_pmd());	/* almost surely new */

	double expect = pow(1 - exp(-2.0 * n / BLOOM_BITS), 2);

	printf("n = %5ld: %5ld of %ld non-members pass (%.1f%%; "
	       "formula %.1f%%)\n", n, fp, probes, 100.0 * fp / probes,
	       100 * expect);
	bloom_free(&b);
}

/*
 * Double buffering.  The walk for generation seq scans only the PMDs that
 * pass the filter for seq, and adds those that are still dense (at least
 * one young PTE per cache line) to the filter for seq + 1, which it
 * emptied first.  Between walks, eviction's look-around may add PMDs it
 * found dense to the same filter.  A PMD that stops being dense is
 * forgotten after one generation, without any deletion.
 */
static void double_buffering(void)
{
	struct bloom_pair b;
	enum { N = 4096 };
	static bool dense[N];

	assert(bloom_init(&b));
	for (long i = 0; i < N; i++) {
		dense[i] = i % 4 == 0;
		bloom_add(&b, 0, pmd(i));	/* first walk: scan everything */
	}
	for (unsigned long seq = 0; seq < 6; seq++) {
		long scanned = 0, kept = 0;

		bloom_reset(&b, seq + 1);
		for (long i = 0; i < N; i++) {
			if (!bloom_test(&b, seq, pmd(i)))
				continue;		/* skip this PTE page */
			scanned++;
			if (dense[i]) {
				bloom_add(&b, seq + 1, pmd(i));
				kept++;
			}
		}
		for (long i = 0; i < N; i++)	/* no dense PMD is lost */
			assert(!dense[i] || bloom_test(&b, seq + 1, pmd(i)) ||
			       !bloom_test(&b, seq, pmd(i)));
		printf("walk %lu: scanned %4ld of %d PMDs, kept %4ld", seq,
		       scanned, N, kept);
		if (seq == 2) {			/* the hot data moves */
			for (long i = 0; i < N; i++)
				dense[i] = i % 4 == 1;
			for (long i = 0; i < N; i++)	/* look-around */
				if (dense[i] && i % 8 == 1)
					bloom_add(&b, seq + 1, pmd(i));
			printf("  <- hot set moves; look-around reports half");
		}
		printf("\n");
	}
	bloom_free(&b);
}

int main(void)
{
	printf("m = %u bits, k = 2\n", BLOOM_BITS);
	false_positives(10000);
	false_positives(20000);
	double_buffering();
	return 0;
}
