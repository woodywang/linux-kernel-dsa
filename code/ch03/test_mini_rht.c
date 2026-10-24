// SPDX-License-Identifier: MIT
/*
 * test_mini_rht.c - randomized tests for mini_rht.c.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -o test_mini_rht test_mini_rht.c
 *   ./test_mini_rht [iterations] [seed]
 *
 * A plain array maps each key to the object that holds it (the model).
 * Every step inserts, removes, runs part of the deferred worker, or looks
 * up a key.  During a lookup the reader hook fires at each step of the
 * walk, and the test then interleaves writer work with the reader:
 * worker steps (moving entries, publishing a table), insertions, removals,
 * and, most important, removing the very object the reader is standing on
 * and inserting it again under a different key, which carries the reader
 * into another chain.  Keys the reader is looking for are never touched
 * while it looks, so the lookup must return exactly what the model says.
 *
 * Old tables are freed only by rht_synchronize(), which the test calls
 * only when no lookup is in progress: that is the "grace period".
 */
#define MINI_RHT_NO_MAIN
#include "mini_rht.c"

#include <assert.h>
#include <string.h>
#include "container_of.h"

#define NOBJ	600
#define NKEYS	2000

struct item {
	uint64_t key;
	bool in;			/* inserted, according to the model */
	struct rht_head node;
};

static struct rhashtable ht;
static struct item items[NOBJ];
static struct item *model[NKEYS];	/* key -> object, or NULL */
static uint64_t protected_key;		/* the key a lookup is seeking */
static long diverted, steps_in_lookups;

static uint64_t free_key(void)
{
	uint64_t k;

	do
		k = rand() % NKEYS;
	while (model[k] || k == protected_key);
	return k;
}

static void do_insert(struct item *it)
{
	it->key = free_key();
	assert(rht_insert(&ht, &it->node) == 0);
	it->in = true;
	model[it->key] = it;
}

static void do_remove(struct item *it)
{
	assert(rht_remove(&ht, &it->node) == 0);
	it->in = false;
	model[it->key] = NULL;
}

/* Writer work done "concurrently" with a lookup, at one of its steps. */
static void hook(struct rht_head *he)
{
	struct item *cur = container_of(he, struct item, node);
	int r = rand() % 100;

	steps_in_lookups++;
	if (r < 40) {
		rht_worker_step(&ht);
	} else if (r < 55 && cur->in && cur->key != protected_key) {
		/* Move the reader's object to another chain (or table). */
		do_remove(cur);
		do_insert(cur);
		diverted++;
	} else if (r < 65) {
		struct item *it = &items[rand() % NOBJ];

		if (it->in && it->key != protected_key)
			do_remove(it);
		else if (!it->in)
			do_insert(it);
	}
}

/* Walk every table: each inserted object exactly once, in its bucket. */
static void check(void)
{
	unsigned int count = 0;
	int seen[NOBJ] = { 0 };

	for (struct bucket_table *tbl = ht.tbl; tbl; tbl = tbl->future_tbl) {
		for (unsigned int b = 0; b < tbl->size; b++) {
			struct rht_head *he = tbl->buckets[b];

			for (; !rht_is_a_nulls(he); he = he->next) {
				struct item *it = container_of(he, struct item,
							       node);

				assert(rht_bucket_index(tbl, it->key) == b);
				assert(it->in && model[it->key] == it);
				seen[it - items]++;
				count++;
			}
			/* Each chain ends in its own bucket's marker. */
			assert(he == NULLS_MARKER(&tbl->buckets[b]));
		}
	}
	for (int i = 0; i < NOBJ; i++)
		assert(seen[i] == items[i].in);
	assert(count == ht.nelems);
	for (uint64_t k = 0; k < NKEYS; k++) {
		void *got = rht_lookup(&ht, k);

		assert(got == model[k]);
	}
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 100000;
	unsigned int seed = argc > 2 ? atoi(argv[2]) : 1;
	long lookups = 0, resizes = 0, max_size = 0;
	unsigned int last_size;

	srand(seed);
	assert(rht_init(&ht, offsetof(struct item, key),
			offsetof(struct item, node), true, seed) == 0);
	protected_key = NKEYS;			/* nothing protected yet */
	last_size = ht.tbl->size;

	for (long it = 0; it < iters; it++) {
		int r = rand() % 100;
		struct item *obj = &items[rand() % NOBJ];

		/* Phases: mostly inserting, then mostly removing, and so on. */
		bool grow = (it / 5000) % 2 == 0;

		if (r < 30) {
			if (!obj->in && (grow || rand() % 3 == 0))
				do_insert(obj);
		} else if (r < 55) {
			if (obj->in && (!grow || rand() % 3 == 0))
				do_remove(obj);
		} else if (r < 75) {
			rht_worker_step(&ht);
		} else {
			uint64_t k = rand() % NKEYS;
			struct item *want = model[k];

			protected_key = k;
			rht_reader_hook = hook;
			void *got = rht_lookup(&ht, k);

			rht_reader_hook = NULL;
			protected_key = NKEYS;
			assert(got == want);
			lookups++;
		}
		if (rand() % 8 == 0)
			rht_synchronize(&ht);	/* no reader is active here */
		if (ht.tbl->size != last_size) {
			resizes++;
			last_size = ht.tbl->size;
			if (last_size > max_size)
				max_size = last_size;
		}
		if (it % 211 == 0 || it == iters - 1)
			check();
	}
	rht_run_worker(&ht);
	check();
	rht_destroy(&ht);
	printf("test_mini_rht: %ld operations, seed %u: ok\n", iters, seed);
	printf("  %ld lookups (%ld interleaved writer steps, %ld readers "
	       "diverted), %ld resizes, largest table %ld\n", lookups,
	       steps_in_lookups, diverted, resizes, max_size);
	return 0;
}
