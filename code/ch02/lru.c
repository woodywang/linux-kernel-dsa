// SPDX-License-Identifier: MIT
/*
 * lru.c - a fixed-capacity LRU cache built from two intrusive lists.
 *
 * Every entry is on two structures at once, through two embedded nodes:
 *   - an hlist bucket of a small hash table (hnode), for lookup by key;
 *   - one list_head recency list (lru), most recently used first.
 * A hit moves the entry to the front with list_move(); a miss on a full
 * cache evicts the entry at the back.  All entries are allocated up front,
 * so lru_get() and lru_put() never allocate and cannot fail.
 *
 * What it leaves out on purpose: locking, resizing, and a good hash
 * function (Chapter 3).
 *
 *   cc -O2 -Wall -o lru lru.c && ./lru
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "list.h"

struct lru_entry {
	unsigned int key;
	int value;
	struct hlist_node hnode;	/* in buckets[key % nbuckets] */
	struct list_head lru;		/* in cache->lru, or on cache->free */
};

struct lru_cache {
	unsigned int nbuckets;
	struct hlist_head *buckets;
	struct list_head lru;		/* front = most recently used */
	struct list_head free;		/* unused entries */
	struct lru_entry *pool;
	unsigned long hits, misses, evictions;
};

static int lru_init(struct lru_cache *c, unsigned int capacity,
		    unsigned int nbuckets)
{
	c->nbuckets = nbuckets;
	c->buckets = calloc(nbuckets, sizeof(*c->buckets));
	c->pool = calloc(capacity, sizeof(*c->pool));
	if (!c->buckets || !c->pool) {
		free(c->buckets);
		free(c->pool);
		return -1;
	}
	INIT_LIST_HEAD(&c->lru);
	INIT_LIST_HEAD(&c->free);
	for (unsigned int i = 0; i < capacity; i++) {
		INIT_HLIST_NODE(&c->pool[i].hnode);
		list_add_tail(&c->pool[i].lru, &c->free);
	}
	c->hits = c->misses = c->evictions = 0;
	return 0;
}

static void lru_destroy(struct lru_cache *c)
{
	free(c->buckets);
	free(c->pool);
}

static struct lru_entry *lru_find(struct lru_cache *c, unsigned int key)
{
	struct lru_entry *e;

	hlist_for_each_entry(e, &c->buckets[key % c->nbuckets], hnode)
		if (e->key == key)
			return e;
	return NULL;
}

/* Look up @key; on a hit, store its value in *value and mark it recent. */
static bool lru_get(struct lru_cache *c, unsigned int key, int *value)
{
	struct lru_entry *e = lru_find(c, key);

	if (!e) {
		c->misses++;
		return false;
	}
	c->hits++;
	list_move(&e->lru, &c->lru);		/* to the front */
	*value = e->value;
	return true;
}

/* Insert or update @key; may evict the least recently used entry. */
static void lru_put(struct lru_cache *c, unsigned int key, int value)
{
	struct lru_entry *e = lru_find(c, key);

	if (!e) {
		if (!list_empty(&c->free)) {
			e = list_first_entry(&c->free, struct lru_entry, lru);
		} else {			/* evict from the back */
			e = list_last_entry(&c->lru, struct lru_entry, lru);
			hlist_del(&e->hnode);
			c->evictions++;
		}
		e->key = key;
		hlist_add_head(&e->hnode, &c->buckets[key % c->nbuckets]);
	}
	e->value = value;
	list_move(&e->lru, &c->lru);		/* from free, back, or middle */
}

#ifndef LRU_NO_MAIN
static void lru_print(struct lru_cache *c)
{
	struct lru_entry *e;

	printf("  recent -> old:");
	list_for_each_entry(e, &c->lru, lru)
		printf(" %u", e->key);
	printf("\n");
}

int main(void)
{
	static const unsigned int trace[] = { 1, 2, 3, 1, 4, 5, 2, 1 };
	struct lru_cache c;
	int v;

	if (lru_init(&c, 3, 4))
		return 1;
	for (unsigned int i = 0; i < sizeof(trace) / sizeof(trace[0]); i++) {
		unsigned int k = trace[i];

		if (lru_get(&c, k, &v)) {
			printf("get %u: hit ", k);
		} else {
			printf("get %u: miss", k);
			lru_put(&c, k, (int)k * 10);
		}
		lru_print(&c);
	}
	printf("hits %lu, misses %lu, evictions %lu\n",
	       c.hits, c.misses, c.evictions);
	lru_destroy(&c);
	return 0;
}
#endif
