// SPDX-License-Identifier: MIT
/*
 * bloom.h - the double-buffered Bloom filters of the multi-gen LRU's
 * page-table walk, in the shape of mm/vmscan.c: m = 2^15 bits, k = 2,
 * and both bit positions cut from one 30-bit multiplicative hash of the
 * item's address.  An item is the address of a page-middle-directory
 * entry, that is, of one page of PTEs.
 *
 * What it leaves out on purpose: allocation failure (the kernel then
 * treats every item as a member), RCU, and atomic bit operations.
 */
#ifndef BLOOM_H
#define BLOOM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BLOOM_FILTER_SHIFT	15
#define NR_BLOOM_FILTERS	2
#define BLOOM_BITS		(1u << BLOOM_FILTER_SHIFT)
#define GOLDEN_RATIO_64		0x61C8864680B583EBull

struct bloom_pair {
	uint64_t *filters[NR_BLOOM_FILTERS];	/* flip each generation */
};

static inline uint32_t hash_64(uint64_t val, unsigned int bits)
{
	return val * GOLDEN_RATIO_64 >> (64 - bits);
}

/* Two keys of 15 bits each from one 30-bit hash. */
static inline void get_item_key(uintptr_t item, uint32_t key[2])
{
	uint32_t hash = hash_64(item, BLOOM_FILTER_SHIFT * 2);

	key[0] = hash & (BLOOM_BITS - 1);
	key[1] = hash >> BLOOM_FILTER_SHIFT;
}

static inline bool test_bit_(uint32_t nr, const uint64_t *map)
{
	return map[nr / 64] >> (nr % 64) & 1;
}

/* The walk for generation seq reads filters[seq % 2] ... */
static inline bool bloom_test(const struct bloom_pair *b, unsigned long seq,
			      uintptr_t item)
{
	const uint64_t *f = b->filters[seq % NR_BLOOM_FILTERS];
	uint32_t key[2];

	get_item_key(item, key);
	return test_bit_(key[0], f) && test_bit_(key[1], f);
}

/* ... and writes the items worth scanning next time into the other. */
static inline void bloom_add(struct bloom_pair *b, unsigned long seq,
			     uintptr_t item)
{
	uint64_t *f = b->filters[seq % NR_BLOOM_FILTERS];
	uint32_t key[2];

	get_item_key(item, key);
	f[key[0] / 64] |= 1ull << (key[0] % 64);
	f[key[1] / 64] |= 1ull << (key[1] % 64);
}

/* No deletion: a filter is emptied wholesale before it is refilled. */
static inline void bloom_reset(struct bloom_pair *b, unsigned long seq)
{
	memset(b->filters[seq % NR_BLOOM_FILTERS], 0, BLOOM_BITS / 8);
}

static inline bool bloom_init(struct bloom_pair *b)
{
	for (int i = 0; i < NR_BLOOM_FILTERS; i++)
		b->filters[i] = calloc(BLOOM_BITS / 64, sizeof(uint64_t));
	return b->filters[0] && b->filters[1];
}

static inline void bloom_free(struct bloom_pair *b)
{
	for (int i = 0; i < NR_BLOOM_FILTERS; i++)
		free(b->filters[i]);
}

#endif
