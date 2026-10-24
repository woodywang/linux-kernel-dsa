// SPDX-License-Identifier: MIT
/*
 * bloom.h - a Bloom filter in the shape of the BPF Bloom filter map
 * (BPF_MAP_TYPE_BLOOM_FILTER), for Chapter 20.
 *
 * The bit array is sized from the expected number of entries n and the
 * number of hash functions k as the kernel sizes it: about n * k * 7/5
 * bits, rounded up to a power of two.  Hash function i is jhash with the
 * seed hash_seed + i.
 *
 * What it leaves out on purpose: atomic bit operations, and the 2^32-bit
 * array the kernel falls back to when n * k is very large.
 */
#ifndef BLOOM_H
#define BLOOM_H

#include <stdint.h>

struct bloom {
	uint32_t value_size;
	uint32_t bitset_mask;		/* number of bits - 1 */
	uint32_t hash_seed;
	uint32_t nr_hash_funcs;
	uint64_t bitset[];
};

struct bloom *bloom_create(uint32_t value_size, uint32_t max_entries,
			   uint32_t nr_hash_funcs, uint32_t seed);
void bloom_destroy(struct bloom *b);
int bloom_push(struct bloom *b, const void *value);
int bloom_peek(const struct bloom *b, const void *value);

#endif
