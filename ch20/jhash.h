// SPDX-License-Identifier: MIT
/*
 * jhash.h - Bob Jenkins' lookup3 hash ("jhash"), the hash the BPF hash map
 * and the BPF Bloom filter map use.  lookup3 is in the public domain; this
 * is an independent C rendering of it for the book, written to produce the
 * same values as the kernel's jhash() and jhash2() for the same inputs, so
 * that the teaching maps can be compared with real kernel maps (see
 * live_maps.c).
 *
 * Byte keys are read in little-endian order, as the kernel does on x86 and
 * arm64.
 */
#ifndef JHASH_H
#define JHASH_H

#include <stdint.h>
#include <string.h>

#define JHASH_INITVAL	0xdeadbeefu

static inline uint32_t rol32(uint32_t w, unsigned int s)
{
	return (w << s) | (w >> (32 - s));
}

#define jhash_mix(a, b, c) do {					\
	a -= c;  a ^= rol32(c, 4);  c += b;			\
	b -= a;  b ^= rol32(a, 6);  a += c;			\
	c -= b;  c ^= rol32(b, 8);  b += a;			\
	a -= c;  a ^= rol32(c, 16); c += b;			\
	b -= a;  b ^= rol32(a, 19); a += c;			\
	c -= b;  c ^= rol32(b, 4);  b += a;			\
} while (0)

#define jhash_final(a, b, c) do {				\
	c ^= b; c -= rol32(b, 14);				\
	a ^= c; a -= rol32(c, 11);				\
	b ^= a; b -= rol32(a, 25);				\
	c ^= b; c -= rol32(b, 16);				\
	a ^= c; a -= rol32(c, 4);				\
	b ^= a; b -= rol32(a, 14);				\
	c ^= b; c -= rol32(b, 24);				\
} while (0)

/* Hash 'len' bytes. */
static inline uint32_t jhash(const void *key, uint32_t len, uint32_t initval)
{
	const uint8_t *k = key;
	uint32_t a, b, c, w[3];

	a = b = c = JHASH_INITVAL + len + initval;
	while (len > 12) {
		memcpy(w, k, 12);
		a += w[0];
		b += w[1];
		c += w[2];
		jhash_mix(a, b, c);
		len -= 12;
		k += 12;
	}
	if (len == 0)
		return c;
	/* The last 1..12 bytes, zero-padded, little-endian. */
	uint8_t tail[12] = { 0 };

	memcpy(tail, k, len);
	a += tail[0] | tail[1] << 8 | tail[2] << 16 | (uint32_t)tail[3] << 24;
	b += tail[4] | tail[5] << 8 | tail[6] << 16 | (uint32_t)tail[7] << 24;
	c += tail[8] | tail[9] << 8 | tail[10] << 16 | (uint32_t)tail[11] << 24;
	jhash_final(a, b, c);
	return c;
}

/* Hash 'n' 32-bit words. */
static inline uint32_t jhash2(const uint32_t *k, uint32_t n, uint32_t initval)
{
	uint32_t a, b, c;

	a = b = c = JHASH_INITVAL + (n << 2) + initval;
	while (n > 3) {
		a += k[0];
		b += k[1];
		c += k[2];
		jhash_mix(a, b, c);
		n -= 3;
		k += 3;
	}
	if (n == 0)
		return c;
	a += k[0];
	if (n > 1)
		b += k[1];
	if (n > 2)
		c += k[2];
	jhash_final(a, b, c);
	return c;
}

/* What the BPF maps call: words if the size allows, bytes otherwise. */
static inline uint32_t map_hash(const void *key, uint32_t len, uint32_t seed)
{
	if (len % 4 == 0) {
		uint32_t w[64];

		if (len <= sizeof(w)) {
			memcpy(w, key, len);	/* the kernel's keys are aligned */
			return jhash2(w, len / 4, seed);
		}
	}
	return jhash(key, len, seed);
}

#endif
