// SPDX-License-Identifier: MIT
/*
 * test_crc.c - randomized tests for crc.h.
 *
 *   ./test_crc [iterations] [seed]
 *
 * Checks the standard check values, then compares the bitwise, bytewise
 * and slice-by-8 versions on random buffers with random initial values,
 * for both CRC-32 and CRC-32C.  Also checks two algebraic facts the
 * chapter relies on: a CRC can be computed piece by piece, and a
 * message followed by its own (uninverted) CRC leaves remainder zero.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crc.h"

static unsigned long long rng_state;

static unsigned long rnd(void)
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return (unsigned long)(rng_state * 2685821657736338717ULL);
}

static uint32_t tab[2][256], t8[2][8][256];

static void check_value(int which, uint32_t poly, uint32_t want)
{
	const uint8_t *msg = (const uint8_t *)"123456789";

	assert(~crc32_bitwise(~0u, msg, 9, poly) == want);
	assert(~crc32_bytewise(~0u, msg, 9, tab[which]) == want);
	assert(~crc32_slice8(~0u, msg, 9, t8[which]) == want);
}

int main(int argc, char **argv)
{
	unsigned long iters = argc > 1 ? strtoul(argv[1], NULL, 0) : 100000;
	const uint32_t poly[2] = { CRC32_POLY_LE, CRC32C_POLY_LE };
	uint8_t buf[300 + 4];

	rng_state = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	if (!rng_state)
		rng_state = 1;
	for (int w = 0; w < 2; w++) {
		crc32_table_init(poly[w], tab[w]);
		crc32_slice8_init(poly[w], t8[w]);
		/* The table really is "CRC of one byte, 8 steps". */
		for (int b = 0; b < 256; b++) {
			uint8_t byte = (uint8_t)b;

			assert(tab[w][b] == crc32_bitwise(0, &byte, 1, poly[w]));
		}
	}
	check_value(0, CRC32_POLY_LE, 0xcbf43926u);
	check_value(1, CRC32C_POLY_LE, 0xe3069283u);

	for (unsigned long it = 0; it < iters; it++) {
		int w = rnd() & 1;
		size_t len = rnd() % 301, cut = len ? rnd() % (len + 1) : 0;
		uint32_t init = (rnd() & 1) ? ~0u : (uint32_t)rnd();

		for (size_t i = 0; i < len; i++)
			buf[i] = (uint8_t)rnd();

		uint32_t a = crc32_bitwise(init, buf, len, poly[w]);
		uint32_t b = crc32_bytewise(init, buf, len, tab[w]);
		uint32_t c = crc32_slice8(init, buf, len, t8[w]);

		assert(a == b && b == c);
		/* Piece by piece gives the same result. */
		uint32_t d = crc32_slice8(init, buf, cut, t8[w]);

		assert(crc32_bytewise(d, buf + cut, len - cut, tab[w]) == a);

		/* message || CRC(message), with no pre/post inversion: 0. */
		uint32_t r = crc32_bytewise(0, buf, len, tab[w]);

		for (int i = 0; i < 4; i++)
			buf[len + i] = (uint8_t)(r >> (8 * i));
		assert(crc32_bytewise(0, buf, len + 4, tab[w]) == 0);
	}
	printf("ok: check values match; %lu random buffers agree "
	       "(bitwise, bytewise, slice-by-8)\n", iters);
	return 0;
}
