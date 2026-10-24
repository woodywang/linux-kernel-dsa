// SPDX-License-Identifier: MIT
/*
 * hash.h - multiplicative ("Fibonacci") hashing of integers, following the
 * design of the kernel's hash_32() / hash_64().
 *
 * Multiply by a large odd constant and keep the HIGH bits of the product:
 * every input bit can influence the high bits, while the low bits of the
 * product depend only on the low bits of the input.  The constants are
 * 2^32 and 2^64 times (1 - phi), where phi = (sqrt(5) - 1) / 2; the kernel
 * uses the same values.
 *
 * These functions are fast and spread ordinary keys well, but they are not
 * keyed: anyone who knows the constant can compute collisions (flood.c).
 */
#ifndef HASH_H
#define HASH_H

#include <stdint.h>

#define GOLDEN_RATIO_32 0x61C88647u
#define GOLDEN_RATIO_64 0x61C8864680B583EBull

/* Hash a 32-bit value into @bits bits (1 <= bits <= 32). */
static inline uint32_t hash_32(uint32_t val, unsigned int bits)
{
	return (uint32_t)(val * GOLDEN_RATIO_32) >> (32 - bits);
}

/* Hash a 64-bit value into @bits bits (1 <= bits <= 32). */
static inline uint32_t hash_64(uint64_t val, unsigned int bits)
{
	return (uint32_t)((val * GOLDEN_RATIO_64) >> (64 - bits));
}

#endif
