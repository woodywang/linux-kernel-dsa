// SPDX-License-Identifier: MIT
/*
 * flood.c - hash flooding against an unkeyed hash, and why a keyed hash
 * stops it.
 *
 * A fixed table of 1024 buckets receives 4096 keys three times:
 *   1. ordinary keys 0..4095, hashed with hash_32();
 *   2. keys crafted by an attacker who knows hash_32()'s multiplier;
 *   3. the same crafted keys, hashed with SipHash under a secret key.
 * For each run it prints the longest chain and the number of key
 * comparisons needed to look up every key once.
 *
 * The SipHash key is a constant here so that the output is reproducible;
 * a real table fills it from a cryptographically secure random source.
 *
 *   cc -O2 -Wall -o flood flood.c && ./flood
 */
#include <stdio.h>
#include <stdint.h>
#include "hashtable.h"
#include "siphash.h"

#define BITS	10
#define NKEYS	4096

struct item {
	uint32_t key;
	struct hlist_node node;
};

static struct item items[NKEYS];

/* The inverse of an odd number modulo 2^32, by Newton's iteration. */
static uint32_t inverse_mod_2_32(uint32_t a)
{
	uint32_t x = a;			/* correct to 3 bits */

	for (int i = 0; i < 4; i++)
		x *= 2 - a * x;		/* each step doubles the correct bits */
	return x;
}

/*
 * The attack: choose keys whose products with GOLDEN_RATIO_32 are 0, 1,
 * 2, ...  Their high bits are all zero, so hash_32() sends every one of
 * them to bucket 0, whatever the table size.
 */
static void craft_keys(uint32_t *keys, int n)
{
	uint32_t inv = inverse_mod_2_32(GOLDEN_RATIO_32);

	for (int i = 0; i < n; i++)
		keys[i] = (uint32_t)i * inv;
}

static const siphash_key_t secret = {
	{ 0x0123456789abcdefULL, 0xfedcba9876543210ULL }
};

static uint32_t sip_bucket(uint32_t key)
{
	return siphash_1u64(key, &secret) & ((1u << BITS) - 1);
}

/* Fill a fresh table, then report the longest chain and the comparisons. */
static void run(const char *name, const uint32_t *keys, int keyed)
{
	DEFINE_HASHTABLE(table, BITS);
	unsigned long cmps = 0;
	int longest = 0;
	unsigned int bkt;
	struct item *it;

	for (int i = 0; i < NKEYS; i++) {
		items[i].key = keys[i];
		if (keyed)
			hlist_add_head(&items[i].node, &table[sip_bucket(keys[i])]);
		else
			hash_add(table, &items[i].node, keys[i]);
	}
	for (bkt = 0; bkt < HASH_SIZE(table); bkt++) {
		int len = 0;

		hlist_for_each_entry(it, &table[bkt], node)
			len++;
		if (len > longest)
			longest = len;
	}
	for (int i = 0; i < NKEYS; i++) {
		struct hlist_head *head = keyed ? &table[sip_bucket(keys[i])] :
			&table[hash_min(keys[i], HASH_BITS(table))];

		hlist_for_each_entry(it, head, node) {
			cmps++;
			if (it->key == keys[i])
				break;
		}
	}
	printf("%-26s %8d %12lu\n", name, longest, cmps);
}

int main(void)
{
	static uint32_t ordinary[NKEYS], crafted[NKEYS];

	for (int i = 0; i < NKEYS; i++)
		ordinary[i] = i;
	craft_keys(crafted, NKEYS);

	printf("%d buckets, %d keys\n", 1 << BITS, NKEYS);
	printf("%-26s %8s %12s\n", "", "longest", "comparisons");
	run("ordinary keys, hash_32", ordinary, 0);
	run("crafted keys,  hash_32", crafted, 0);
	run("crafted keys,  SipHash", crafted, 1);
	return 0;
}
