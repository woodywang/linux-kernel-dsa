// SPDX-License-Identifier: MIT
/*
 * test_spsc.c - randomized single-threaded tests for spsc.h.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -o test_spsc test_spsc.c
 *   ./test_spsc [iterations] [seed]
 *
 * The producer always writes consecutive sequence numbers, so the model of
 * the ring is just two counters: how many elements were produced and how
 * many consumed.  After every operation the ring's length and free space
 * must match the model, and every element read must carry the expected
 * sequence number in every byte.  Rings of several sizes and element
 * sizes are tested, with indices starting at 0 and just below UINT_MAX,
 * so that they wrap many times.  (Concurrency is tested by stress_spsc.c.)
 */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include "spsc.h"

#define MAXE	16		/* largest element size tested */
#define MAXN	1100		/* largest batch */

static uint64_t rng;

static unsigned int rnd(unsigned int n)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (unsigned int)(rng % n);
}

/* Element k: every byte derived from k, so torn or misplaced copies show. */
static void make(unsigned char *e, unsigned int esize, uint64_t k)
{
	for (unsigned int j = 0; j < esize; j++)
		e[j] = (unsigned char)(k * 31 + j * 7 + (k >> 8));
}

static void expect(const unsigned char *e, unsigned int esize, uint64_t k)
{
	unsigned char want[MAXE];

	make(want, esize, k);
	assert(memcmp(e, want, esize) == 0);
}

static unsigned char buf[MAXN * MAXE];

static void run(unsigned int size, unsigned int esize, unsigned int start,
		long iters)
{
	struct spsc r;
	uint64_t produced = 0, consumed = 0;

	assert(spsc_init_at(&r, size, esize, start) == 0);
	for (long i = 0; i < iters; i++) {
		unsigned int n = rnd(size + 4), got;

		switch (rnd(6)) {
		case 0:			/* put one */
			make(buf, esize, produced);
			if (spsc_put(&r, buf))
				produced++;
			else
				assert(produced - consumed == size);
			break;
		case 1:			/* get one */
			if (spsc_get(&r, buf))
				expect(buf, esize, consumed++);
			else
				assert(produced == consumed);
			break;
		case 2:			/* put a batch */
			for (unsigned int j = 0; j < n; j++)
				make(buf + j * esize, esize, produced + j);
			got = spsc_in(&r, buf, n);
			assert(got == (n < size - (produced - consumed) ?
				       n : size - (produced - consumed)));
			produced += got;
			break;
		case 3:			/* get a batch */
		case 4: {		/* or only peek at it */
			bool remove = rnd(2);

			got = remove ? spsc_out(&r, buf, n) : spsc_peek(&r, buf, n);
			assert(got == (n < produced - consumed ?
				       n : produced - consumed));
			for (unsigned int j = 0; j < got; j++)
				expect(buf + j * esize, esize, consumed + j);
			if (remove)
				consumed += got;
			break;
		}
		case 5:			/* lengths */
			break;
		}
		assert(produced - consumed <= size);
		assert(spsc_len(&r) == produced - consumed);
		assert(spsc_avail(&r) == size - (produced - consumed));
		assert(load_relaxed(&r.in) == (unsigned int)(start + produced));
		assert(load_relaxed(&r.out) == (unsigned int)(start + consumed));
	}
	spsc_free(&r);
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 100000;
	unsigned int sizes[] = { 2, 8, 64, 1024 }, esizes[] = { 1, 4, 12 };
	struct spsc bad;
	int configs = 0;

	rng = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	rng = rng * 0x9E3779B97F4A7C15ull + 1;

	/* Sizes that are not powers of two are refused. */
	assert(spsc_init(&bad, 0, 1) < 0);
	assert(spsc_init(&bad, 1, 1) < 0);
	assert(spsc_init(&bad, 6, 1) < 0);
	assert(spsc_init(&bad, 1000, 4) < 0);

	for (unsigned int s = 0; s < 4; s++)
		for (unsigned int e = 0; e < 3; e++) {
			unsigned int starts[] = { 0, UINT_MAX - 2,
						  UINT_MAX - 3 * sizes[s] };

			for (unsigned int k = 0; k < 3; k++) {
				run(sizes[s], esizes[e], starts[k], iters);
				configs++;
			}
		}
	printf("test_spsc: %d configurations x %ld operations: ok\n",
	       configs, iters);
	return 0;
}
