// SPDX-License-Identifier: MIT
/*
 * test_pcpu_counter.c - randomized single-threaded test of pcpu_counter.h.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -pthread -o test_pcpu_counter \
 *      test_pcpu_counter.c
 *   ./test_pcpu_counter [steps] [seed]
 *
 * Adds random amounts through random shards and keeps the exact value in
 * a plain int64_t.  After every step it checks that pc_sum() is exact,
 * that every shard's delta is below the batch, that pc_read() is within
 * pc_error_bound() of the exact value, and that pc_compare() agrees with
 * the model.  Several batch sizes are tried, including 1 (every add
 * folds) and a large one (almost nothing folds).
 */
#include <inttypes.h>
#include <stdio.h>
#include "pcpu_counter.h"

static uint64_t rng;

static uint64_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

static void fail(const char *what, long step)
{
	fprintf(stderr, "step %ld: %s\n", step, what);
	exit(1);
}

static void run(long steps, int shards, int32_t batch)
{
	struct pcpu_counter pc;
	int64_t exact = 1000;
	long folds = 0;

	if (pc_init(&pc, exact, shards, batch))
		exit(1);
	for (long i = 0; i < steps; i++) {
		int me = (int)(rnd() % shards);
		int64_t span = 2 * (int64_t)batch + 3;
		int64_t amount = (int64_t)(rnd() % (2 * span + 1)) - span;
		int64_t before = pc_read(&pc);

		if (rnd() % 8 == 0)
			amount = (int64_t)(rnd() % 3) - 1;	/* +-1, 0 */
		pc_add(&pc, me, amount);
		exact += amount;
		folds += pc_read(&pc) != before;

		if (pc_sum(&pc) != exact)
			fail("pc_sum() is not exact", i);
		for (int s = 0; s < shards; s++)
			if (abs64(relaxed(&pc.shards[s].delta)) >= batch)
				fail("a shard reached the batch", i);
		if (abs64(pc_read(&pc) - exact) > pc_error_bound(&pc))
			fail("pc_read() outside the error bound", i);
		int64_t rhs = exact + (int64_t)(rnd() % 7) - 3;
		if (rnd() % 2)
			rhs = exact + (int64_t)(rnd() % (4 * pc_error_bound(&pc) + 1))
			      - 2 * pc_error_bound(&pc);
		if (pc_compare(&pc, rhs) != (exact > rhs) - (exact < rhs))
			fail("pc_compare() disagrees with the model", i);
	}
	printf("test_pcpu_counter: %ld adds, %d shards, batch %d: "
	       "%ld folds, sum exact\n", steps, shards, batch, folds);
	pc_destroy(&pc);
}

int main(int argc, char **argv)
{
	long steps = argc > 1 ? atol(argv[1]) : 100000;
	uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;

	rng = seed * 0x9E3779B97F4A7C15ull + 1;
	run(steps, 4, 1);
	run(steps, 8, 32);
	run(steps, 3, 1000);
	run(steps, 64, 7);
	return 0;
}
