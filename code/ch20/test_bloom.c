// SPDX-License-Identifier: MIT
/*
 * test_bloom.c - check bloom.c and measure its false-positive rate.
 *
 *   ./test_bloom
 *
 * For several (n, k), push the 32-bit values 0..n-1, check that every one
 * of them is found (no false negatives), then peek one million values that
 * were never pushed and compare the share that pass with the usual
 * estimate (1 - e^(-kn/m))^k.  The seed is 0, as with BPF_F_ZERO_SEED, so
 * the output is deterministic (Section 20.7 quotes it) and can be compared
 * with a kernel map (live_maps.c).
 */
#include "bloom.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>

#define PROBES	1000000

static void measure(uint32_t n, uint32_t k)
{
	struct bloom *b = bloom_create(4, n, k, 0);
	uint32_t m, pass = 0;

	assert(b);
	m = b->bitset_mask + 1;
	for (uint32_t v = 0; v < n; v++)
		assert(bloom_push(b, &v) == 0);
	for (uint32_t v = 0; v < n; v++)
		assert(bloom_peek(b, &v) == 0);		/* no false negatives */
	for (uint32_t v = n; v < n + PROBES; v++)
		pass += bloom_peek(b, &v) == 0;

	double p = (double)pass / PROBES;
	double f = pow(1 - exp(-(double)k * n / m), k);
	double sigma = sqrt(f * (1 - f) / PROBES);

	printf("n = %5u, k = %u: m = %6u bits (%4.1f per entry); "
	       "%5.2f%% pass (formula %5.2f%%)\n", n, k, m, (double)m / n,
	       100 * p, 100 * f);
	/* Within five standard deviations, plus 5% for the approximation. */
	assert(fabs(p - f) <= 5 * sigma + 0.05 * f);
	bloom_destroy(b);
}

int main(void)
{
	measure(10000, 5);
	measure(10000, 3);
	measure(15000, 3);
	measure(16000, 3);

	/* max_entries only sizes the array; pushing more is allowed. */
	struct bloom *b = bloom_create(4, 100, 3, 0);

	assert(b);
	for (uint32_t v = 0; v < 1000; v++)
		assert(bloom_push(b, &v) == 0);
	for (uint32_t v = 0; v < 1000; v++)
		assert(bloom_peek(b, &v) == 0);
	bloom_destroy(b);
	return 0;
}
