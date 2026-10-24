// SPDX-License-Identifier: MIT
/*
 * bloom.c - a Bloom filter in the shape of the BPF Bloom filter map.
 * See bloom.h for what it leaves out.
 */
#include "bloom.h"
#include "jhash.h"

#include <errno.h>
#include <stdlib.h>

struct bloom *bloom_create(uint32_t value_size, uint32_t max_entries,
			   uint32_t nr_hash_funcs, uint32_t seed)
{
	uint64_t nr_bits;
	struct bloom *b;

	if (!value_size || !max_entries || nr_hash_funcs > 15)
		return NULL;
	if (!nr_hash_funcs)
		nr_hash_funcs = 5;		/* the kernel's default */

	/* m = n * k / ln 2, with 7/5 standing in for 1/ln 2 ... */
	nr_bits = (uint64_t)max_entries * nr_hash_funcs / 5 * 7;
	if (nr_bits > 1ull << 31)
		return NULL;		/* the kernel would use 2^32 bits */
	/* ... rounded up to a power of two, at least one word. */
	if (nr_bits <= 64)
		nr_bits = 64;
	else
		while (nr_bits & (nr_bits - 1))
			nr_bits += nr_bits & -nr_bits;

	b = calloc(1, sizeof(*b) + nr_bits / 8);
	if (!b)
		return NULL;
	b->value_size = value_size;
	b->bitset_mask = nr_bits - 1;
	b->hash_seed = seed;		/* random unless BPF_F_ZERO_SEED */
	b->nr_hash_funcs = nr_hash_funcs;
	return b;
}

void bloom_destroy(struct bloom *b)
{
	free(b);
}

/* Hash function number i: the same jhash with a different seed. */
static uint32_t hash(const struct bloom *b, const void *value, uint32_t i)
{
	return map_hash(value, b->value_size, b->hash_seed + i) &
	       b->bitset_mask;
}

int bloom_push(struct bloom *b, const void *value)
{
	for (uint32_t i = 0; i < b->nr_hash_funcs; i++) {
		uint32_t h = hash(b, value, i);

		b->bitset[h / 64] |= 1ull << (h % 64);
	}
	return 0;
}

/* 0: possibly present; -ENOENT: definitely absent. */
int bloom_peek(const struct bloom *b, const void *value)
{
	for (uint32_t i = 0; i < b->nr_hash_funcs; i++) {
		uint32_t h = hash(b, value, i);

		if (!(b->bitset[h / 64] & (1ull << (h % 64))))
			return -ENOENT;
	}
	return 0;
}
