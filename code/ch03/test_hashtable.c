// SPDX-License-Identifier: MIT
/*
 * test_hashtable.c - randomized tests for hashtable.h, plus checks of the
 * hash functions in hash.h and siphash.h.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -o test_hashtable test_hashtable.c
 *   ./test_hashtable [iterations] [seed]
 *
 * Objects with small, possibly repeated keys are added to and removed from
 * two fixed tables (32-bit and 64-bit keys).  A plain array of counts is
 * the model: after every step, the number of objects with a given key that
 * hash_for_each_possible() finds must match it, and a full hash_for_each()
 * must see every object exactly once.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "hashtable.h"
#include "siphash.h"

#define NOBJ	2000
#define NKEYS	300

struct obj32 {
	uint32_t key;
	int seen;
	struct hlist_node node;
};

struct obj64 {
	uint64_t key;
	struct hlist_node node;
};

static struct obj32 o32[NOBJ];
static struct obj64 o64[NOBJ];
static int count32[NKEYS], count64[NKEYS];
static DEFINE_HASHTABLE(t32, 6);
static DEFINE_HASHTABLE(t64, 8);

/* A 64-bit key whose low bits repeat, to exercise hash_64()'s high bits. */
static uint64_t key64(int k)
{
	return (uint64_t)k << 40 | 0x5555;
}

static void check_hash_functions(void)
{
	/* SipHash-2-4 test vector (reference implementation, as used by the
	 * kernel's siphash KUnit test): key 00 01 .. 0f, message 00 01 .. 07. */
	const siphash_key_t k = { { 0x0706050403020100ULL,
				    0x0f0e0d0c0b0a0908ULL } };

	assert(siphash_1u64(0x0706050403020100ULL, &k) ==
	       0x93f5f5799a932462ULL);

	/* The multipliers are odd, so multiplication is a bijection. */
	assert(GOLDEN_RATIO_32 & 1);
	assert(GOLDEN_RATIO_64 & 1);
	for (uint32_t v = 0; v < 100000; v++) {
		assert(hash_32(v, 6) < 64);
		assert(hash_64((uint64_t)v << 32, 10) < 1024);
		assert(hash_32(v, 32) == (uint32_t)(v * GOLDEN_RATIO_32));
	}
}

static void check_tables(void)
{
	unsigned int bkt;
	struct obj32 *p;
	struct obj64 *q;
	int total = 0, expect = 0;

	for (int k = 0; k < NKEYS; k++) {
		int n32 = 0, n64 = 0;

		hash_for_each_possible(t32, p, node, (uint32_t)k)
			n32 += p->key == (uint32_t)k;
		hash_for_each_possible(t64, q, node, key64(k))
			n64 += q->key == key64(k);
		assert(n32 == count32[k]);
		assert(n64 == count64[k]);
		expect += count32[k];
	}
	for (int i = 0; i < NOBJ; i++)
		o32[i].seen = 0;
	hash_for_each(t32, bkt, p, node) {
		p->seen++;
		total++;
	}
	assert(total == expect);
	for (int i = 0; i < NOBJ; i++)
		assert(o32[i].seen == !hlist_unhashed(&o32[i].node));
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 100000;
	unsigned int seed = argc > 2 ? atoi(argv[2]) : 1;
	unsigned int bkt;
	struct obj32 *p;

	srand(seed);
	check_hash_functions();
	for (int i = 0; i < NOBJ; i++) {
		INIT_HLIST_NODE(&o32[i].node);
		INIT_HLIST_NODE(&o64[i].node);
	}
	for (long it = 0; it < iters; it++) {
		int i = rand() % NOBJ, k = rand() % NKEYS;

		if (hlist_unhashed(&o32[i].node)) {
			o32[i].key = k;
			hash_add(t32, &o32[i].node, o32[i].key);
			count32[k]++;
			o64[i].key = key64(k);
			hash_add(t64, &o64[i].node, o64[i].key);
			count64[k]++;
		} else {
			count32[o32[i].key]--;
			hash_del(&o32[i].node);
			count64[o64[i].key >> 40]--;
			hash_del(&o64[i].node);
		}
		if (it % 101 == 0 || it == iters - 1)
			check_tables();
	}

	/* "break" inside hash_for_each() must leave both loops. */
	int visited = 0, stored = 0;

	for (int k = 0; k < NKEYS; k++)
		stored += count32[k];

	hash_for_each(t32, bkt, p, node) {
		visited++;
		break;
	}
	assert(visited == (stored ? 1 : 0));
	printf("test_hashtable: %ld operations, seed %u: ok\n", iters, seed);
	return 0;
}
