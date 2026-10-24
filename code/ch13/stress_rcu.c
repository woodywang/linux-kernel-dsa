// SPDX-License-Identifier: MIT
/*
 * stress_rcu.c - readers and updaters on an RCU-protected hash table.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -pthread \
 *      -o stress_rcu stress_rcu.c urcu.c
 *   ./stress_rcu [updates-per-updater] [seed]
 *
 * READERS threads look keys up and walk whole buckets under
 * rcu_read_lock(), with no other synchronization.  UPDATERS threads
 * insert, delete and replace (copy-update) objects under one writer lock
 * and free what they remove with call_rcu() or, now and then, with
 * synchronize_rcu() followed by free().  Every object carries a checksum
 * of its key and version; a reader that sees a half-initialized or
 * recycled object fails the check, and one that touches freed memory is
 * caught by AddressSanitizer (or ThreadSanitizer, in the TSAN build).
 *
 * Each updater owns the keys k with k % UPDATERS == its id and keeps a
 * model of them; at the end the table must equal the union of the models.
 * Built with -DURCU_BROKEN, grace periods don't wait and ASan must report
 * a heap-use-after-free.
 */
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "rculist.h"

#define READERS		3
#define UPDATERS	2
#define NKEYS		512
#define NBUCKETS	64

struct obj {
	struct hlist_node node;
	uint64_t key;
	uint64_t version;
	uint64_t check;
	struct rcu_head rh;
};

static struct hlist_head table[NBUCKETS];
static pthread_mutex_t writer_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool stop;
static _Atomic long n_alloc, n_free;
static long updates;
static uint64_t seed;

static uint64_t mix(uint64_t key, uint64_t version)
{
	uint64_t x = key * 0x9E3779B97F4A7C15ull ^ version;

	return x ^ (x >> 29);
}

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

static void fail(const char *what, const struct obj *o)
{
	fprintf(stderr, "FAIL: %s: key %" PRIu64 " version %" PRIu64 "\n",
		what, o->key, o->version);
	abort();
}

/* ---- the table -------------------------------------------------------- */

static struct obj *obj_new(uint64_t key, uint64_t version)
{
	struct obj *o = malloc(sizeof(*o));

	if (!o)
		abort();
	o->key = key;
	o->version = version;
	o->check = mix(key, version);
	n_alloc++;
	return o;
}

static void obj_free_rcu(struct rcu_head *rh)
{
	struct obj *o = container_of(rh, struct obj, rh);

	o->check = 0;			/* poison, then free */
	free(o);
	n_free++;
}

/* Reader side: no locks, only rcu_read_lock() around the call. */
static struct obj *lookup(uint64_t key)
{
	struct obj *o;

	hlist_for_each_entry_rcu(o, &table[bucket_of(key)], node)
		if (o->key == key)
			return o;
	return NULL;
}

/* ---- readers ---------------------------------------------------------- */

static void *reader(void *arg)
{
	uint64_t s = seed ^ (uintptr_t)arg ^ 0x1234567;
	long found = 0, walks = 0;

	rcu_register_thread();
	while (!stop) {
		uint64_t key = rnd(&s) % NKEYS;
		struct obj *o;

		rcu_read_lock();
		o = lookup(key);
		if (o) {
			if (o->check != mix(o->key, o->version))
				fail("torn or recycled object", o);
			if (rnd(&s) % 4 == 0) {		/* nested section */
				rcu_read_lock();
				sched_yield();		/* hold on to o a while */
				rcu_read_unlock();
			}
			if (o->key != key || o->check != mix(key, o->version))
				fail("object changed under the reader", o);
			found++;
		}
		rcu_read_unlock();

		if (rnd(&s) % 8 == 0) {			/* walk a whole bucket */
			unsigned int b = rnd(&s) % NBUCKETS;

			rcu_read_lock();
			hlist_for_each_entry_rcu(o, &table[b], node)
				if (bucket_of(o->key) != b ||
				    o->check != mix(o->key, o->version))
					fail("bad object in bucket walk", o);
			rcu_read_unlock();
			walks++;
		}
	}
	rcu_unregister_thread();
	(void)found;
	(void)walks;
	return NULL;
}

/* ---- updaters --------------------------------------------------------- */

static void *updater(void *arg)
{
	unsigned int id = (unsigned int)(uintptr_t)arg;
	uint64_t s = seed * 31 + id + 1;
	static _Thread_local uint64_t model[NKEYS];	/* 0: absent, else version */

	for (long i = 0; i < updates; i++) {
		uint64_t key = (rnd(&s) % (NKEYS / UPDATERS)) * UPDATERS + id;
		unsigned int op = rnd(&s) % 16;
		struct obj *o, *n;

		pthread_mutex_lock(&writer_lock);
		o = lookup(key);	/* the lock keeps the table still */
		if ((o != NULL) != (model[key] != 0) ||
		    (o && o->version != model[key]))
			fail("table disagrees with model", o ? o : &(struct obj){ .key = key });
		if (!o) {				/* insert */
			n = obj_new(key, i + 1);
			hlist_add_head_rcu(&n->node, &table[bucket_of(key)]);
			model[key] = n->version;
			pthread_mutex_unlock(&writer_lock);
		} else if (op < 8) {			/* replace: copy-update */
			n = obj_new(key, o->version + 1);
			hlist_replace_rcu(&o->node, &n->node);
			model[key] = n->version;
			pthread_mutex_unlock(&writer_lock);
			call_rcu(&o->rh, obj_free_rcu);
		} else {				/* delete */
			hlist_del_rcu(&o->node);
			model[key] = 0;
			pthread_mutex_unlock(&writer_lock);
			if (op == 15) {
				synchronize_rcu();	/* wait, then free */
				obj_free_rcu(&o->rh);
			} else {
				call_rcu(&o->rh, obj_free_rcu);
			}
		}
	}
	/* Hand the model to main() through the table check below. */
	pthread_mutex_lock(&writer_lock);
	for (uint64_t k = id; k < NKEYS; k += UPDATERS) {
		struct obj *o = lookup(k);

		if ((o != NULL) != (model[k] != 0) || (o && o->version != model[k]))
			fail("final table disagrees with model",
			     o ? o : &(struct obj){ .key = k });
	}
	pthread_mutex_unlock(&writer_lock);
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t r[READERS], u[UPDATERS];
	struct rcu_stats st;
	long left = 0;

	updates = argc > 1 ? atol(argv[1]) : 200000;
	seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;

	for (uintptr_t i = 0; i < READERS; i++)
		pthread_create(&r[i], NULL, reader, (void *)i);
	for (uintptr_t i = 0; i < UPDATERS; i++)
		pthread_create(&u[i], NULL, updater, (void *)i);
	for (int i = 0; i < UPDATERS; i++)
		pthread_join(u[i], NULL);
	stop = true;
	for (int i = 0; i < READERS; i++)
		pthread_join(r[i], NULL);

	/* Empty the table, then wait for every callback. */
	for (int b = 0; b < NBUCKETS; b++)
		while (table[b].first) {
			struct obj *o = container_of(table[b].first, struct obj, node);

			hlist_del_rcu(&o->node);
			call_rcu(&o->rh, obj_free_rcu);
			left++;
		}
	rcu_barrier();
	rcu_get_stats(&st);
	rcu_shutdown();
	if (n_alloc != n_free) {
		fprintf(stderr, "FAIL: %ld allocated, %ld freed\n",
			(long)n_alloc, (long)n_free);
		return 1;
	}
	printf("stress_rcu: %ld updates by %d updaters, %d readers: ok "
	       "(%ld objects left at the end)\n",
	       updates * UPDATERS, UPDATERS, READERS, left);
	printf("  %lu callbacks in %lu batches; %lu grace periods for %lu "
	       "waits, %lu of which found theirs already over\n",
	       st.callbacks, st.batches, st.grace_periods, st.gp_waits,
	       st.gp_shared);
	return 0;
}
