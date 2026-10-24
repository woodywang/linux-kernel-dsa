// SPDX-License-Identifier: MIT
/*
 * test_htab.c - randomized test of htab.c against a direct-address model.
 *
 *   ./test_htab STEPS SEED
 *
 * Each step runs a lookup, update (with a random flag) or delete on a
 * random simulated CPU and compares the result with the model.  Every 97
 * steps it checks the invariants: chains end at their own nulls marker,
 * every element is in exactly one place (a chain, a free list, or a
 * spare), and no malloc() has happened since the map was created.
 */
#include "htab.h"
#include "jhash.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rng;

static uint64_t rnd(void)			/* xorshift64 */
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

static void make_key(uint8_t *k, uint32_t key_size, uint32_t id)
{
	memset(k, 0, key_size);
	for (uint32_t i = 0; i < key_size && i < 4; i++)
		k[i] = id >> (8 * i);
	if (key_size > 4)
		k[key_size - 1] = 0x5a;		/* make the tail matter */
}

static void check_invariants(struct htab *h, int count)
{
	uint32_t total = h->max_entries + NR_CPUS;
	char *seen = calloc(total, 1);
	int in_chains = 0, in_free = 0;

	assert(seen);
	for (uint32_t i = 0; i < h->n_buckets; i++) {
		struct htab_elem *l;

		for (l = h->buckets[i].first; !is_nulls(l); l = l->next) {
			size_t off = (char *)l - h->elems;

			assert(off % h->elem_size == 0 && off / h->elem_size < total);
			assert((l->hash & (h->n_buckets - 1)) == i);
			assert(!seen[off / h->elem_size]);
			seen[off / h->elem_size] = 1;
			in_chains++;
		}
		assert(nulls_value(l) == i);
	}
	for (int cpu = 0; cpu < NR_CPUS; cpu++) {
		struct htab_elem *l;

		for (l = h->freelist[cpu]; l; l = l->free_next) {
			size_t idx = ((char *)l - h->elems) / h->elem_size;

			assert(!seen[idx]);
			seen[idx] = 1;
			in_free++;
		}
		size_t idx = ((char *)h->extra[cpu] - h->elems) / h->elem_size;

		assert(!seen[idx]);
		seen[idx] = 1;
	}
	assert(in_chains == count);
	assert(in_chains + in_free + NR_CPUS == (int)total);
	free(seen);
}

static void run(uint32_t key_size, uint32_t max_entries, long steps)
{
	uint32_t universe = 3 * max_entries;
	struct htab *h = htab_create(key_size, 8, max_entries,
				     (uint32_t)rnd());
	char *present = calloc(universe, 1);
	uint64_t *val = calloc(universe, sizeof(*val));
	uint8_t key[16];
	int count = 0;
	long full = 0;

	assert(h && present && val);
	unsigned long mallocs = htab_nr_mallocs;

	for (long s = 0; s < steps; s++) {
		uint32_t id = rnd() % universe;
		int op = rnd() % 100;
		uint64_t v = rnd(), *got;
		int ret;

		htab_cpu = rnd() % NR_CPUS;
		make_key(key, key_size, id);
		if (op < 40) {
			got = htab_lookup(h, key);
			assert(!got == !present[id]);
			assert(!got || *got == val[id]);
		} else if (op < 75) {
			int flags = rnd() % 3;

			ret = htab_update(h, key, &v, flags);
			if (present[id] && flags == BPF_NOEXIST) {
				assert(ret == -EEXIST);
			} else if (!present[id] && flags == BPF_EXIST) {
				assert(ret == -ENOENT);
			} else if (!present[id] && count == (int)max_entries) {
				assert(ret == -E2BIG);
				full++;
			} else {
				assert(ret == 0);
				count += !present[id];
				present[id] = 1;
				val[id] = v;
			}
		} else {
			ret = htab_delete(h, key);
			assert(ret == (present[id] ? 0 : -ENOENT));
			count -= present[id];
			present[id] = 0;
		}
		if (s % 97 == 0)
			check_invariants(h, count);
	}
	check_invariants(h, count);
	assert(htab_nr_mallocs == mallocs);	/* nothing after creation */
	printf("key %2u bytes, max_entries %4u: %ld steps ok, "
	       "%ld inserts refused as full\n", key_size, max_entries, steps,
	       full);
	htab_destroy(h);
	free(present);
	free(val);
}

int main(int argc, char **argv)
{
	long steps = argc > 1 ? atol(argv[1]) : 200000;

	rng = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	rng = rng * 0x9E3779B97F4A7C15ull + 1;

	/* jhash() over bytes equals jhash2() over the same words. */
	for (int i = 0; i < 1000; i++) {
		uint32_t w[5] = { rnd(), rnd(), rnd(), rnd(), rnd() };
		uint32_t n = i % 6, seed = rnd();

		assert(jhash(w, 4 * n, seed) == jhash2(w, n, seed));
	}
	run(4, 8, steps / 4);
	run(4, 64, steps / 4);
	run(6, 100, steps / 4);
	run(12, 1000, steps / 4);
	return 0;
}
