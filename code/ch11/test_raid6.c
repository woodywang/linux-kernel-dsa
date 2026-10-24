// SPDX-License-Identifier: MIT
/*
 * test_raid6.c - tests for raid6.h.
 *
 *   ./test_raid6 [iterations] [seed]
 *
 * First the field: shift-and-add multiplication agrees with the
 * logarithm tables for all 65,536 pairs, every nonzero element has an
 * inverse, and the word-at-a-time multiply-by-2 agrees with the byte
 * version.  Then randomized erasure tests: build a random stripe of 4 to
 * 40 disks, compute P and Q (both ways), destroy two random blocks
 * (data or parity) by filling them with garbage, recover, and compare
 * with a saved copy.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "raid6.h"

static unsigned long long rng_state;

static unsigned long rnd(void)
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return (unsigned long)(rng_state * 2685821657736338717ULL);
}

#define MAX_DISKS 40

static void test_field(void)
{
	for (int a = 0; a < 256; a++) {
		for (int b = 0; b < 256; b++) {
			uint8_t want = (a && b) ?
				gf_exp[gf_log[a] + gf_log[b]] : 0;

			assert(gf_mul((uint8_t)a, (uint8_t)b) == want);
		}
		if (a)
			assert(gf_mul((uint8_t)a, gf_inv((uint8_t)a)) == 1);
	}
	for (int i = 0; i < 100000; i++) {
		uint64_t v = ((uint64_t)rnd() << 32) ^ rnd(), w = 0;

		for (int k = 0; k < 8; k++)
			w |= (uint64_t)gf_mul2((uint8_t)(v >> (8 * k))) << (8 * k);
		assert(gf_mul2_x8(v) == w);
	}
}

int main(int argc, char **argv)
{
	unsigned long iters = argc > 1 ? strtoul(argv[1], NULL, 0) : 20000;
	static uint8_t blk[MAX_DISKS][512], saved[MAX_DISKS][512];
	uint8_t *ptrs[MAX_DISKS];
	unsigned long cases[4] = { 0 };

	rng_state = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	if (!rng_state)
		rng_state = 1;
	gf_init();
	test_field();

	for (unsigned long it = 0; it < iters; it++) {
		int disks = 4 + rnd() % (MAX_DISKS - 3);
		size_t bytes = 8 * (1 + rnd() % 64);

		for (int z = 0; z < disks; z++) {
			ptrs[z] = blk[z];
			for (size_t i = 0; i < bytes; i++)
				blk[z][i] = (uint8_t)rnd();
		}
		raid6_gen_syndrome(disks, bytes, ptrs);
		for (int z = 0; z < disks; z++)
			memcpy(saved[z], blk[z], bytes);
		raid6_gen_syndrome_x8(disks, bytes, ptrs);
		for (int z = disks - 2; z < disks; z++)
			assert(!memcmp(saved[z], blk[z], bytes));

		int a = rnd() % disks, b;

		do
			b = rnd() % disks;
		while (b == a);
		for (size_t i = 0; i < bytes; i++) {
			blk[a][i] = (uint8_t)rnd();
			blk[b][i] = (uint8_t)rnd();
		}
		raid6_recover(disks, bytes, a, b, ptrs);
		for (int z = 0; z < disks; z++)
			assert(!memcmp(saved[z], blk[z], bytes));

		int hi = a > b ? a : b, lo = a < b ? a : b;

		cases[hi == disks - 1 ? (lo == disks - 2 ? 0 : 1)
				      : (hi == disks - 2 ? 2 : 3)]++;
	}
	printf("ok: field checks; %lu stripes recovered "
	       "(P+Q %lu, data+Q %lu, data+P %lu, data+data %lu)\n",
	       iters, cases[0], cases[1], cases[2], cases[3]);
	return 0;
}
