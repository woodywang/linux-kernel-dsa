// SPDX-License-Identifier: MIT
/*
 * typesafe.c - type-stable memory (SLAB_TYPESAFE_BY_RCU) and the lookup
 * pattern it forces: nulls-terminated chains, a reference count that is
 * taken only if it is not zero, and a second look at the key.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -pthread \
 *      -o typesafe typesafe.c urcu.c
 *   ./typesafe [updates-per-updater] [seed]
 *
 * The object cache below frees objects the way a SLAB_TYPESAFE_BY_RCU
 * slab cache does: a freed object goes straight back on the free list and
 * may be handed out again at once, for a new key, while lock-free readers
 * are still looking at it.  Only the memory *underneath* the objects (the
 * slabs) is returned after a grace period.  The cache keeps its free-list
 * link in a field readers never look at, as SLUB does for such caches.
 *
 * Readers look keys up without locks; updaters insert and delete.  A
 * reader that gets an object back must hold the key it asked for, and the
 * key must not change while the reader holds its reference.  Built with
 * -DNO_RECHECK the lookup skips the second look at the key, and the test
 * must catch readers walking off with somebody else's object.
 */
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "rculist.h"

#define READERS		3
#define UPDATERS	2
#define NKEYS		256
#define NBUCKETS	16
#define SLAB_OBJS	32

struct tobj {
	struct hlist_nulls_node node;
	_Atomic unsigned int refcnt;	/* 0 while free; the table holds 1 */
	_Atomic uint64_t key;
	struct tobj *free_next;		/* the cache's link, not node.next */
};

/* ---- a type-stable object cache ----------------------------------------- */

struct slab {
	struct slab *next;
	struct tobj objs[SLAB_OBJS];
};

static struct {
	pthread_mutex_t lock;
	struct tobj *free;
	struct slab *slabs;
	long nslabs;
} cache = { .lock = PTHREAD_MUTEX_INITIALIZER };

static struct tobj *cache_alloc(void)
{
	struct tobj *o;

	pthread_mutex_lock(&cache.lock);
	if (!cache.free) {
		struct slab *s = calloc(1, sizeof(*s));

		if (!s)
			abort();
		s->next = cache.slabs;
		cache.slabs = s;
		cache.nslabs++;
		for (int i = 0; i < SLAB_OBJS; i++) {
			s->objs[i].node.next = NULLS_MARKER(0);
			s->objs[i].free_next = cache.free;
			cache.free = &s->objs[i];
		}
	}
	o = cache.free;
	cache.free = o->free_next;
	pthread_mutex_unlock(&cache.lock);
	return o;
}

/* No grace period: the object may be reused immediately. */
static void cache_free(struct tobj *o)
{
	pthread_mutex_lock(&cache.lock);
	o->free_next = cache.free;
	cache.free = o;
	pthread_mutex_unlock(&cache.lock);
}

/* The memory itself goes away only after a grace period. */
static void cache_destroy(void)
{
	synchronize_rcu();
	while (cache.slabs) {
		struct slab *s = cache.slabs;

		cache.slabs = s->next;
		free(s);
	}
}

/* ---- reference counts ----------------------------------------------- */

static bool get_unless_zero(struct tobj *o)
{
	unsigned int c = atomic_load_explicit(&o->refcnt, memory_order_relaxed);

	do {
		if (c == 0)
			return false;	/* free, or about to be */
	} while (!atomic_compare_exchange_weak_explicit(&o->refcnt, &c, c + 1,
			memory_order_acquire, memory_order_relaxed));
	return true;
}

static void put(struct tobj *o)
{
	if (atomic_fetch_sub_explicit(&o->refcnt, 1, memory_order_acq_rel) == 1)
		cache_free(o);
}

/* ---- the table -------------------------------------------------------- */

static struct hlist_nulls_head table[NBUCKETS];
static pthread_mutex_t writer_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool stop;
static _Atomic long n_restart_nulls, n_restart_key, n_wrong;
static long updates;
static uint64_t seed;

static uint64_t rnd(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

static unsigned int bucket_of(uint64_t key)
{
	return (unsigned int)(key * 0x61C88647u) % NBUCKETS;
}

static uint64_t key_of(struct tobj *o)
{
	return atomic_load_explicit(&o->key, memory_order_relaxed);
}

/* Widen the race windows now and then. */
static void reader_pause(uint64_t *s)
{
	if (rnd(s) % 8 == 0)
		sched_yield();
}

/* Find key and return the object with a reference held, or NULL. */
static struct tobj *lookup_get(uint64_t key, uint64_t *s)
{
	unsigned long slot = bucket_of(key);
	struct hlist_nulls_node *pos;
	struct tobj *o;

	rcu_read_lock();
begin:
	for (pos = rcu_dereference(table[slot].first); !is_a_nulls(pos);
	     pos = rcu_dereference(pos->next)) {
		o = container_of(pos, struct tobj, node);
		if (key_of(o) != key)
			continue;
		reader_pause(s);
		if (!get_unless_zero(o))
			goto begin;		/* being freed: start over */
#ifndef NO_RECHECK
		if (key_of(o) != key) {		/* reused for another key */
			n_restart_key++;
			put(o);
			goto begin;
		}
#endif
		rcu_read_unlock();
		return o;
	}
	if (get_nulls_value(pos) != slot) {	/* ended in another chain */
		n_restart_nulls++;
		goto begin;
	}
	rcu_read_unlock();
	return NULL;
}

static void *reader(void *arg)
{
	uint64_t s = seed ^ (uintptr_t)arg ^ 0xfeed;

	rcu_register_thread();
	while (!stop) {
		uint64_t key = rnd(&s) % NKEYS;
		struct tobj *o = lookup_get(key, &s);

		if (!o)
			continue;
		uint64_t got = key_of(o);

		if (got != key)
			n_wrong++;		/* somebody else's object */
		reader_pause(&s);
		if (key_of(o) != got) {		/* our reference must pin it */
			fprintf(stderr, "FAIL: key changed under a reference\n");
			abort();
		}
		put(o);
	}
	rcu_unregister_thread();
	return NULL;
}

/* Writer side, under writer_lock: no references needed. */
static struct tobj *find_locked(uint64_t key)
{
	struct hlist_nulls_node *pos;

	for (pos = table[bucket_of(key)].first; !is_a_nulls(pos); pos = pos->next) {
		struct tobj *o = container_of(pos, struct tobj, node);

		if (key_of(o) == key)
			return o;
	}
	return NULL;
}

static void *updater(void *arg)
{
	unsigned int id = (unsigned int)(uintptr_t)arg;
	uint64_t s = seed * 77 + id + 1;
	static _Thread_local bool model[NKEYS];

	for (long i = 0; i < updates; i++) {
		uint64_t key = (rnd(&s) % (NKEYS / UPDATERS)) * UPDATERS + id;
		struct tobj *o;

		pthread_mutex_lock(&writer_lock);
		o = find_locked(key);
		if ((o != NULL) != model[key]) {
			fprintf(stderr, "FAIL: key %" PRIu64 " %s\n", key,
				o ? "present" : "missing");
			abort();
		}
		if (!o) {			/* insert: reuse a free object */
			pthread_mutex_unlock(&writer_lock);
			o = cache_alloc();
			atomic_store_explicit(&o->key, key, memory_order_relaxed);
			/* Initialize first, then make the object gettable. */
			atomic_store_explicit(&o->refcnt, 1, memory_order_release);
			pthread_mutex_lock(&writer_lock);
			hlist_nulls_add_head_rcu(&o->node, &table[bucket_of(key)]);
			model[key] = true;
			pthread_mutex_unlock(&writer_lock);
		} else {			/* delete: drop the table's ref */
			hlist_nulls_del_rcu(&o->node);
			model[key] = false;
			pthread_mutex_unlock(&writer_lock);
			put(o);
		}
	}
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t r[READERS], u[UPDATERS];

	updates = argc > 1 ? atol(argv[1]) : 200000;
	seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	for (unsigned long i = 0; i < NBUCKETS; i++)
		table[i].first = NULLS_MARKER(i);

	for (uintptr_t i = 0; i < READERS; i++)
		pthread_create(&r[i], NULL, reader, (void *)i);
	for (uintptr_t i = 0; i < UPDATERS; i++)
		pthread_create(&u[i], NULL, updater, (void *)i);
	for (int i = 0; i < UPDATERS; i++)
		pthread_join(u[i], NULL);
	stop = true;
	for (int i = 0; i < READERS; i++)
		pthread_join(r[i], NULL);

	for (unsigned long b = 0; b < NBUCKETS; b++)
		while (!is_a_nulls(table[b].first)) {
			struct tobj *o = container_of(table[b].first,
						      struct tobj, node);

			hlist_nulls_del_rcu(&o->node);
			put(o);
		}
	long nslabs = cache.nslabs;

	cache_destroy();
	rcu_shutdown();
	if (n_wrong) {
		fprintf(stderr, "FAIL: readers got the wrong object %ld times\n",
			(long)n_wrong);
		return 1;
	}
	printf("typesafe: %ld updates, %d readers, %ld slabs of %d objects: ok\n",
	       updates * UPDATERS, READERS, nslabs, SLAB_OBJS);
	printf("  restarts: %ld at a foreign nulls marker, %ld after the key "
	       "changed\n", (long)n_restart_nulls, (long)n_restart_key);
	return 0;
}
