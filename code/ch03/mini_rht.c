// SPDX-License-Identifier: MIT
/*
 * mini_rht.c - a small teaching version of the kernel's resizable hash
 * table (rhashtable).
 *
 * What it shows:
 *   - chains that end in a "nulls marker" naming their bucket, so that a
 *     reader carried into another chain can tell and start again;
 *   - lookups that search the current table and then any future table;
 *   - insertions that go to the newest table and only *request* a resize
 *     (at 75% load) from a deferred worker;
 *   - the worker, which allocates a table of the new size, moves entries
 *     one at a time from the tail of each old chain, publishes the new
 *     table, and retires the old one until a "grace period" has passed;
 *   - a chain-length limit (16) that forces a rehash with a new random
 *     seed, the table's defence against hash flooding.
 *
 * What it leaves out on purpose: real concurrency (the program is single-
 * threaded; the tests interleave the worker's steps with a reader's steps
 * through a hook instead), per-bucket locks, RCU itself, nested tables for
 * allocation failures, size limits, iteration, and duplicate-key lists.
 *
 *   cc -O2 -Wall -o mini_rht mini_rht.c && ./mini_rht
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "siphash.h"

#define RHT_ELASTICITY		16u	/* longest chain before a rehash */
#define HASH_DEFAULT_SIZE	64u

struct rht_head {
	struct rht_head *next;
};

struct bucket_table {
	unsigned int size;		/* a power of two */
	uint32_t hash_rnd;		/* random seed for this table */
	unsigned int rehash;		/* worker: next old bucket to empty */
	struct bucket_table *future_tbl; /* table being filled, if resizing */
	struct bucket_table *retired;	/* on the list waiting to be freed */
	struct rht_head *buckets[];
};

struct rhashtable {
	struct bucket_table *tbl;	/* the current table */
	unsigned int nelems;
	size_t key_offset, head_offset;	/* where the u64 key and node live */
	unsigned int min_size;
	bool automatic_shrinking;
	bool work_pending;		/* the kernel calls schedule_work() */
	struct bucket_table *retired;	/* old tables awaiting a grace period */
	uint64_t rng;			/* stands in for get_random_u32() */
	long moved;			/* statistics for the demo */
};

/* Test hook: called at every step of a lookup.  NULL in the demo. */
void (*rht_reader_hook)(struct rht_head *he);

static inline void reader_step(struct rht_head *he)
{
	if (rht_reader_hook)
		rht_reader_hook(he);
}

/*
 * The end of every chain is an odd "pointer" that names its bucket.  Real
 * nodes are aligned, so their low bit is always 0.
 */
#define NULLS_MARKER(bkt)	((struct rht_head *)((uintptr_t)(bkt) | 1))

static inline bool rht_is_a_nulls(const struct rht_head *p)
{
	return (uintptr_t)p & 1;
}

static inline uint64_t rht_key(const struct rhashtable *ht,
			       const struct rht_head *he)
{
	return *(const uint64_t *)((const char *)he - ht->head_offset +
				   ht->key_offset);
}

static inline void *rht_obj(const struct rhashtable *ht,
			    const struct rht_head *he)
{
	return (char *)he - ht->head_offset;
}

/*
 * The kernel's default is jhash() with the table's seed.  Here SipHash
 * keyed with the seed plays that role; either way, an attacker who does
 * not know the seed cannot predict which bucket a key lands in.
 */
static inline uint32_t rht_hash(uint64_t key, uint32_t seed)
{
	const siphash_key_t k = { { seed, ~(uint64_t)seed } };

	return (uint32_t)siphash_1u64(key, &k);
}

static inline unsigned int rht_bucket_index(const struct bucket_table *tbl,
					    uint64_t key)
{
	return rht_hash(key, tbl->hash_rnd) & (tbl->size - 1);
}

static uint32_t rht_random(struct rhashtable *ht)
{
	ht->rng ^= ht->rng << 13;
	ht->rng ^= ht->rng >> 7;
	ht->rng ^= ht->rng << 17;
	return (uint32_t)ht->rng;
}

static struct bucket_table *bucket_table_alloc(struct rhashtable *ht,
					       unsigned int size)
{
	struct bucket_table *tbl;

	tbl = calloc(1, sizeof(*tbl) + size * sizeof(tbl->buckets[0]));
	if (!tbl)
		return NULL;
	tbl->size = size;
	tbl->hash_rnd = rht_random(ht);
	for (unsigned int i = 0; i < size; i++)
		tbl->buckets[i] = NULLS_MARKER(&tbl->buckets[i]);
	return tbl;
}

static unsigned int roundup_pow_of_two(unsigned int n)
{
	unsigned int p = 1;

	while (p < n)
		p <<= 1;
	return p;
}

/* Grow when the load factor exceeds 75%... */
static bool rht_grow_above_75(const struct rhashtable *ht,
			      const struct bucket_table *tbl)
{
	return ht->nelems > tbl->size / 4 * 3;
}

/* ...shrink below 30%, and stop inserting into a table above 100%. */
static bool rht_shrink_below_30(const struct rhashtable *ht,
				const struct bucket_table *tbl)
{
	return ht->nelems < tbl->size * 3 / 10 && tbl->size > ht->min_size;
}

static bool rht_grow_above_100(const struct rhashtable *ht,
			       const struct bucket_table *tbl)
{
	return ht->nelems > tbl->size;
}

int rht_init(struct rhashtable *ht, size_t key_offset, size_t head_offset,
	     bool automatic_shrinking, uint64_t seed)
{
	*ht = (struct rhashtable){
		.key_offset = key_offset,
		.head_offset = head_offset,
		.min_size = 4,
		.automatic_shrinking = automatic_shrinking,
		.rng = seed | 1,
	};
	ht->tbl = bucket_table_alloc(ht, HASH_DEFAULT_SIZE);
	return ht->tbl ? 0 : -ENOMEM;
}

/* --------------------------------------------------------------------- */
/* Lookup: search the current table, then any table a resize is filling. */

void *rht_lookup(struct rhashtable *ht, uint64_t key)
{
	struct bucket_table *tbl = ht->tbl;
	struct rht_head *he;
	unsigned int hash;

restart:
	hash = rht_bucket_index(tbl, key);
	do {
		for (he = tbl->buckets[hash]; !rht_is_a_nulls(he);
		     he = he->next) {
			reader_step(he);
			if (rht_key(ht, he) == key)
				return rht_obj(ht, he);
		}
		/* We ended in some other bucket's chain: walk ours again. */
	} while (he != NULLS_MARKER(&tbl->buckets[hash]));

	tbl = tbl->future_tbl;		/* a resize may have moved it there */
	if (tbl)
		goto restart;
	return NULL;
}

/* --------------------------------------------------------------------- */
/* Insertion.                                                             */

/*
 * Called by an insert that cannot wait for the worker: the chain is too
 * long (flooding) or the table is over 100% full.  Attach a new table now
 * and let the worker move the entries.
 */
static int rht_insert_rehash(struct rhashtable *ht, struct bucket_table *tbl)
{
	unsigned int size = tbl->size;
	struct bucket_table *new_tbl;

	if (rht_grow_above_75(ht, tbl))
		size *= 2;
	else if (ht->tbl != tbl)
		return -EBUSY;		/* do not start a second rehash */

	new_tbl = bucket_table_alloc(ht, size);	/* with a new seed */
	if (!new_tbl)
		return -ENOMEM;
	tbl->future_tbl = new_tbl;
	ht->work_pending = true;
	return 0;
}

int rht_insert(struct rhashtable *ht, struct rht_head *obj)
{
	uint64_t key = rht_key(ht, obj);

	for (;;) {
		struct bucket_table *tbl = ht->tbl;
		struct rht_head **bkt, *he;
		unsigned int len;
		int err;

		/* Check every table for the key; stop at the newest one. */
		for (;;) {
			bkt = &tbl->buckets[rht_bucket_index(tbl, key)];
			len = 0;
			for (he = *bkt; !rht_is_a_nulls(he); he = he->next) {
				if (rht_key(ht, he) == key)
					return -EEXIST;
				len++;
			}
			if (!tbl->future_tbl)
				break;
			tbl = tbl->future_tbl;
		}

		if (len >= RHT_ELASTICITY || rht_grow_above_100(ht, tbl)) {
			err = rht_insert_rehash(ht, tbl);
			if (err)
				return err;
			continue;	/* now insert into the new table */
		}

		obj->next = *bkt;	/* push onto the head of the chain */
		*bkt = obj;
		ht->nelems++;
		if (rht_grow_above_75(ht, tbl))
			ht->work_pending = true;	/* schedule_work() */
		return 0;
	}
}

int rht_remove(struct rhashtable *ht, struct rht_head *obj)
{
	uint64_t key = rht_key(ht, obj);

	for (struct bucket_table *tbl = ht->tbl; tbl; tbl = tbl->future_tbl) {
		struct rht_head **pprev, *he;

		pprev = &tbl->buckets[rht_bucket_index(tbl, key)];
		for (he = *pprev; !rht_is_a_nulls(he); he = he->next) {
			if (he != obj) {
				pprev = &he->next;
				continue;
			}
			/* Unlink, but leave obj->next alone for readers. */
			*pprev = obj->next;
			ht->nelems--;
			if (ht->automatic_shrinking &&
			    rht_shrink_below_30(ht, tbl))
				ht->work_pending = true;
			return 0;
		}
	}
	return -ENOENT;
}

/* --------------------------------------------------------------------- */
/* The deferred worker.                                                   */

/* Move the last entry of old bucket @b into the future table. */
static int rht_rehash_one(struct rhashtable *ht, struct bucket_table *old_tbl,
			  unsigned int b)
{
	struct bucket_table *new_tbl = old_tbl->future_tbl;
	struct rht_head **pprev = &old_tbl->buckets[b];
	struct rht_head *entry = *pprev, *next;
	unsigned int new_hash;

	if (rht_is_a_nulls(entry))
		return -ENOENT;			/* this bucket is done */
	while (!rht_is_a_nulls(next = entry->next)) {
		pprev = &entry->next;
		entry = next;
	}
	/* entry is the tail; next is the old bucket's nulls marker. */
	new_hash = rht_bucket_index(new_tbl, rht_key(ht, entry));
	entry->next = new_tbl->buckets[new_hash];
	new_tbl->buckets[new_hash] = entry;	/* publish in the new table */
	*pprev = next;				/* then unlink from the old */
	ht->moved++;
	return 0;
}

/* Decide whether to resize, and attach a table of the new size. */
static void rht_start_resize(struct rhashtable *ht)
{
	struct bucket_table *tbl = ht->tbl;
	unsigned int size;

	if (rht_grow_above_75(ht, tbl)) {
		size = tbl->size * 2;
	} else if (ht->automatic_shrinking && rht_shrink_below_30(ht, tbl)) {
		size = ht->nelems ? roundup_pow_of_two(ht->nelems * 3 / 2) : 0;
		if (size < ht->min_size)
			size = ht->min_size;
		if (size >= tbl->size)
			return;
	} else {
		return;
	}
	tbl->future_tbl = bucket_table_alloc(ht, size);
	/* (The kernel reschedules the worker if this allocation fails.) */
}

/* One unit of the worker's job.  Returns false when there is nothing to do. */
bool rht_worker_step(struct rhashtable *ht)
{
	struct bucket_table *old_tbl = ht->tbl;

	if (!old_tbl->future_tbl) {
		if (!ht->work_pending)
			return false;
		ht->work_pending = false;
		rht_start_resize(ht);
		return old_tbl->future_tbl != NULL;
	}
	for (; old_tbl->rehash < old_tbl->size; old_tbl->rehash++)
		if (rht_rehash_one(ht, old_tbl, old_tbl->rehash) == 0)
			return true;

	/* Every chain is empty: publish the new table, retire the old. */
	ht->tbl = old_tbl->future_tbl;
	old_tbl->retired = ht->retired;
	ht->retired = old_tbl;		/* the kernel uses call_rcu() */
	return true;
}

void rht_run_worker(struct rhashtable *ht)
{
	while (rht_worker_step(ht))
		;
}

/* The grace period has ended: no reader can still hold an old table. */
void rht_synchronize(struct rhashtable *ht)
{
	while (ht->retired) {
		struct bucket_table *tbl = ht->retired;

		ht->retired = tbl->retired;
		free(tbl);
	}
}

void rht_destroy(struct rhashtable *ht)
{
	struct bucket_table *tbl = ht->tbl;

	rht_synchronize(ht);
	while (tbl) {
		struct bucket_table *next = tbl->future_tbl;

		free(tbl);
		tbl = next;
	}
}

/* Longest chain in the current table (for the demo and the tests). */
unsigned int rht_longest_chain(const struct rhashtable *ht)
{
	const struct bucket_table *tbl = ht->tbl;
	unsigned int longest = 0;

	for (unsigned int b = 0; b < tbl->size; b++) {
		unsigned int len = 0;

		for (const struct rht_head *he = tbl->buckets[b];
		     !rht_is_a_nulls(he); he = he->next)
			len++;
		if (len > longest)
			longest = len;
	}
	return longest;
}

/* --------------------------------------------------------------------- */
/* Demo                                                                   */

#ifndef MINI_RHT_NO_MAIN
struct item {
	uint64_t key;
	struct rht_head node;
};

#define N	1000

static struct item items[N];

/* Run the worker to completion and report any resize it performed. */
static void run_worker(struct rhashtable *ht)
{
	unsigned int before = ht->tbl->size;
	uint32_t seed = ht->tbl->hash_rnd;

	rht_run_worker(ht);
	rht_synchronize(ht);
	if (ht->tbl->size != before)
		printf("  %4u elements: %4u -> %4u buckets\n", ht->nelems,
		       before, ht->tbl->size);
	else if (ht->tbl->hash_rnd != seed)
		printf("  %4u elements: rehashed at %u buckets with a new seed\n",
		       ht->nelems, before);
}

int main(void)
{
	struct rhashtable ht;

	rht_init(&ht, offsetof(struct item, key), offsetof(struct item, node),
		 true, 2026);

	printf("insert %d keys (start: %u buckets):\n", N, ht.tbl->size);
	for (int i = 0; i < N; i++) {
		items[i].key = i;
		rht_insert(&ht, &items[i].node);
		run_worker(&ht);
	}
	printf("remove all but 100:\n");
	for (int i = 0; i < N - 100; i++) {
		rht_remove(&ht, &items[i].node);
		run_worker(&ht);
	}
	printf("key 950 %s, key 5 %s; longest chain %u; %ld entries moved\n",
	       rht_lookup(&ht, 950) ? "found" : "missing",
	       rht_lookup(&ht, 5) ? "found" : "missing",
	       rht_longest_chain(&ht), ht.moved);

	/*
	 * Flooding: suppose an attacker has learned the current seed.  Trying
	 * keys until 40 of them land in bucket 0 takes a few thousand guesses.
	 */
	static struct item evil[40];
	uint64_t guess = 1000000;

	printf("flood: 40 keys that collide under the current seed:\n");
	for (int i = 0; i < 40; i++) {
		while (rht_bucket_index(ht.tbl, guess) != 0)
			guess++;
		evil[i].key = guess++;
	}
	for (int i = 0; i < 40; i++) {
		rht_insert(&ht, &evil[i].node);
		run_worker(&ht);
	}
	printf("longest chain %u\n", rht_longest_chain(&ht));
	rht_destroy(&ht);
	return 0;
}
#endif
