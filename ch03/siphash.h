// SPDX-License-Identifier: MIT
/*
 * siphash.h - SipHash-2-4 of a single 64-bit value under a 128-bit secret
 * key (the kernel's siphash_1u64()).  SipHash was designed by Jean-Philippe
 * Aumasson and Daniel J. Bernstein; this is a plain transcription of the
 * algorithm for one 8-byte message, checked in test_hashtable.c against a
 * published test vector.
 */
#ifndef SIPHASH_H
#define SIPHASH_H

#include <stdint.h>

typedef struct {
	uint64_t key[2];
} siphash_key_t;

static inline uint64_t rol64(uint64_t w, unsigned int s)
{
	return (w << s) | (w >> (64 - s));
}

#define SIPROUND(v0, v1, v2, v3) do {					\
	v0 += v1; v1 = rol64(v1, 13); v1 ^= v0; v0 = rol64(v0, 32);	\
	v2 += v3; v3 = rol64(v3, 16); v3 ^= v2;				\
	v0 += v3; v3 = rol64(v3, 21); v3 ^= v0;				\
	v2 += v1; v1 = rol64(v1, 17); v1 ^= v2; v2 = rol64(v2, 32);	\
} while (0)

static inline uint64_t siphash_1u64(uint64_t m, const siphash_key_t *k)
{
	uint64_t v0 = 0x736f6d6570736575ULL ^ k->key[0];
	uint64_t v1 = 0x646f72616e646f6dULL ^ k->key[1];
	uint64_t v2 = 0x6c7967656e657261ULL ^ k->key[0];
	uint64_t v3 = 0x7465646279746573ULL ^ k->key[1];
	uint64_t b = 8ULL << 56;	/* message length in the top byte */

	v3 ^= m;			/* one message block, two rounds */
	SIPROUND(v0, v1, v2, v3);
	SIPROUND(v0, v1, v2, v3);
	v0 ^= m;
	v3 ^= b;			/* the final block, two rounds */
	SIPROUND(v0, v1, v2, v3);
	SIPROUND(v0, v1, v2, v3);
	v0 ^= b;
	v2 ^= 0xff;			/* finalization, four rounds */
	SIPROUND(v0, v1, v2, v3);
	SIPROUND(v0, v1, v2, v3);
	SIPROUND(v0, v1, v2, v3);
	SIPROUND(v0, v1, v2, v3);
	return v0 ^ v1 ^ v2 ^ v3;
}

#endif
