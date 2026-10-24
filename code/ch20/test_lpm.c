// SPDX-License-Identifier: MIT
/*
 * test_lpm.c - randomized test of lpm.c against a brute-force model.
 *
 *   ./test_lpm STEPS SEED
 *
 * The model is a plain array of (prefix, length, value) entries; a lookup
 * scans all of them for the longest matching prefix.  Prefixes are drawn
 * from a few clusters, so that they nest and share long common parts, and
 * the bits after each prefix are random, since the kernel does not mask
 * them either.  Every 61 steps the test checks the trie's invariants and
 * walks it with lpm_get_next_key().
 */
#include "lpm.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
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

struct entry {
	struct lpm_key key;			/* bits after prefixlen are 0 */
	uint32_t value;
};

static struct entry *model;
static int model_n;
static size_t dsize;
static uint8_t clusters[8][LPM_DATA_MAX];

static bool prefix_eq(const uint8_t *a, const uint8_t *b, uint32_t bits)
{
	for (uint32_t i = 0; i < bits; i++)
		if (extract_bit(a, i) != extract_bit(b, i))
			return false;
	return true;
}

static void mask(struct lpm_key *k)
{
	for (uint32_t i = k->prefixlen; i < 8 * dsize; i++)
		k->data[i / 8] &= ~(1 << (7 - i % 8));
}

static void random_key(struct lpm_key *k, bool full_length)
{
	const uint8_t *base = clusters[rnd() % 8];
	uint32_t keep = rnd() % (8 * dsize + 1);	/* bits from the cluster */

	memset(k, 0, sizeof(*k));
	for (size_t i = 0; i < dsize; i++)
		k->data[i] = rnd();
	for (uint32_t i = 0; i < keep; i++) {
		uint8_t m = 1 << (7 - i % 8);

		k->data[i / 8] = (k->data[i / 8] & ~m) | (base[i / 8] & m);
	}
	k->prefixlen = full_length ? 8 * dsize : rnd() % (8 * dsize + 1);
}

static int model_find(const struct lpm_key *k)
{
	for (int i = 0; i < model_n; i++)
		if (model[i].key.prefixlen == k->prefixlen &&
		    prefix_eq(model[i].key.data, k->data, k->prefixlen))
			return i;
	return -1;
}

static const struct entry *model_lookup(const struct lpm_key *k)
{
	const struct entry *best = NULL;

	for (int i = 0; i < model_n; i++) {
		const struct entry *e = &model[i];

		if (e->key.prefixlen <= k->prefixlen &&
		    prefix_eq(e->key.data, k->data, e->key.prefixlen) &&
		    (!best || e->key.prefixlen > best->key.prefixlen))
			best = e;
	}
	return best;
}

static size_t check_node(const struct lpm_trie *t, const struct lpm_node *n,
			 size_t *real)
{
	size_t nodes = 1;

	if (n->flags & LPM_FLAG_IM) {
		assert(n->child[0] && n->child[1]);
	} else {
		struct lpm_key k = { .prefixlen = n->prefixlen };
		int i;

		memcpy(k.data, n->data, dsize);
		i = model_find(&k);
		assert(i >= 0 && model[i].value == n->value);
		(*real)++;
	}
	for (int c = 0; c < 2; c++) {
		const struct lpm_node *ch = n->child[c];

		if (!ch)
			continue;
		assert(ch->prefixlen > n->prefixlen);
		assert(prefix_eq(ch->data, n->data, n->prefixlen));
		assert(extract_bit(ch->data, n->prefixlen) == c);
		nodes += check_node(t, ch, real);
	}
	return nodes;
}

static void check_trie(const struct lpm_trie *t)
{
	size_t real = 0, nodes = t->root ? check_node(t, t->root, &real) : 0;
	struct lpm_key k, prev[256];
	int n = 0, ret;

	assert(real == t->n_entries && (int)real == model_n);
	assert(nodes == t->n_nodes);
	assert(nodes <= 2 * real);	/* at most real - 1 branch points */

	/* Iterate: every entry once, more specific before less specific. */
	for (ret = lpm_get_next_key(t, NULL, &k); ret == 0;
	     ret = lpm_get_next_key(t, &prev[n - 1], &k)) {
		assert(n < model_n && n < 256);
		assert(model_find(&k) >= 0);
		for (int j = 0; j < n; j++) {
			assert(prev[j].prefixlen != k.prefixlen ||
			       !prefix_eq(prev[j].data, k.data, k.prefixlen));
			/* an ancestor never comes before its descendant */
			assert(!(k.prefixlen > prev[j].prefixlen &&
				 prefix_eq(k.data, prev[j].data,
					   prev[j].prefixlen)));
		}
		prev[n++] = k;
	}
	assert(ret == -ENOENT && n == model_n);
}

static void run(size_t data_size, size_t max_entries, long steps)
{
	struct lpm_trie t;
	struct lpm_key k;
	long hits = 0, lookups = 0, full = 0;

	dsize = data_size;
	model = calloc(max_entries, sizeof(*model));
	model_n = 0;
	assert(model);
	for (int c = 0; c < 8; c++)
		for (size_t i = 0; i < dsize; i++)
			clusters[c][i] = rnd();
	lpm_init(&t, data_size, max_entries);

	for (long s = 0; s < steps; s++) {
		int op = rnd() % 100, i, ret;

		if (op < 45) {
			random_key(&k, rnd() % 4 != 0);
			const struct entry *e = model_lookup(&k);
			uint32_t *v = lpm_lookup(&t, &k);

			assert(!e == !v);
			assert(!v || *v == e->value);
			hits += !!v;
			lookups++;
		} else if (op < 80) {
			int flags = rnd() % 3;
			uint32_t value = rnd();

			random_key(&k, false);
			i = model_find(&k);
			ret = lpm_update(&t, &k, value, flags);
			if (i >= 0 && flags == BPF_NOEXIST) {
				assert(ret == -EEXIST);
			} else if (i < 0 && flags == BPF_EXIST) {
				assert(ret == -ENOENT);
			} else if (i < 0 && model_n == (int)max_entries) {
				assert(ret == -ENOSPC);
				full++;
			} else {
				assert(ret == 0);
				if (i < 0) {
					i = model_n++;
					model[i].key = k;
					mask(&model[i].key);
				}
				model[i].value = value;
			}
		} else {
			/* Delete something present half of the time. */
			if (model_n && rnd() % 2) {
				k = model[rnd() % model_n].key;
				for (uint32_t b = k.prefixlen; b < 8 * dsize; b++)
					if (rnd() % 2)	/* garbage after it */
						k.data[b / 8] |= 1 << (7 - b % 8);
			} else {
				random_key(&k, false);
			}
			i = model_find(&k);
			ret = lpm_delete(&t, &k);
			assert(ret == (i >= 0 ? 0 : -ENOENT));
			if (i >= 0)
				model[i] = model[--model_n];
		}
		if (s % 61 == 0)
			check_trie(&t);
	}
	check_trie(&t);
	printf("%2zu-byte keys, max_entries %3zu: %ld steps ok, "
	       "%ld of %ld lookups matched, %ld inserts refused as full\n",
	       data_size, max_entries, steps, hits, lookups, full);
	lpm_destroy(&t);
	assert(t.n_nodes == 0);
	free(model);
}

int main(int argc, char **argv)
{
	long steps = argc > 1 ? atol(argv[1]) : 200000;

	rng = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	rng = rng * 0x9E3779B97F4A7C15ull + 1;
	run(4, 200, steps / 2);		/* IPv4 */
	run(4, 20, steps / 8);		/* IPv4, often full */
	run(1, 50, steps / 8);
	run(3, 100, steps / 8);
	run(16, 200, steps / 8);	/* IPv6 */
	return 0;
}
