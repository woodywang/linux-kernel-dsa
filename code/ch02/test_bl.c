// SPDX-License-Identifier: MIT
/*
 * test_bl.c - stress test for list_bl.h: several threads add and remove
 * nodes in a small hash table whose only locks are bit 0 of each bucket's
 * head pointer.
 *
 *   cc -O2 -Wall -pthread -fsanitize=address,undefined -o test_bl test_bl.c
 *   ./test_bl [operations per thread] [seed]
 *
 * Each thread owns its own objects, so the model is simple: an object is
 * either on the chain the thread put it on or on none.  Under each bucket
 * lock the thread also walks the whole chain and checks every pprev link
 * and the lock bit, so a broken lock shows up as a broken chain.  At the
 * end the chains must hold exactly the objects the threads think they do.
 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "list_bl.h"

#define THREADS	4
#define PER	64		/* objects per thread */
#define NB	8		/* buckets: few, so threads collide */

struct obj {
	int owner;
	int bucket;		/* -1 when on no chain */
	struct hlist_bl_node node;
};

static struct hlist_bl_head table[NB];
static struct obj objs[THREADS][PER];
static long ops = 100000;
static unsigned int seed = 1;

/* Called with the bucket locked. */
static void check_chain(struct hlist_bl_head *h)
{
	struct hlist_bl_node *pos, **expect = &h->first;
	struct obj *o;

	assert(hlist_bl_is_locked(h));
	hlist_bl_for_each_entry(o, pos, h, node) {
		assert(pos->pprev == expect);
		assert(&table[o->bucket] == h);
		expect = &pos->next;
	}
}

static void *worker(void *arg)
{
	int t = (int)(long)arg;
	unsigned int r = seed * 7919u + (unsigned int)t;

	for (long i = 0; i < ops; i++) {
		struct obj *o = &objs[t][rand_r(&r) % PER];
		int b = o->bucket >= 0 ? o->bucket : (int)(rand_r(&r) % NB);
		struct hlist_bl_head *h = &table[b];

		hlist_bl_lock(h);
		if (o->bucket < 0) {
			o->bucket = b;
			hlist_bl_add_head(&o->node, h);
		} else {
			hlist_bl_del_init(&o->node);
			o->bucket = -1;
		}
		check_chain(h);
		hlist_bl_unlock(h);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t tid[THREADS];
	int count[NB] = { 0 };

	if (argc > 1)
		ops = atol(argv[1]);
	if (argc > 2)
		seed = (unsigned int)atoi(argv[2]);
	for (int b = 0; b < NB; b++)
		INIT_HLIST_BL_HEAD(&table[b]);
	for (int t = 0; t < THREADS; t++)
		for (int i = 0; i < PER; i++) {
			objs[t][i].owner = t;
			objs[t][i].bucket = -1;
			INIT_HLIST_BL_NODE(&objs[t][i].node);
		}
	for (long t = 0; t < THREADS; t++)
		assert(!pthread_create(&tid[t], NULL, worker, (void *)t));
	for (int t = 0; t < THREADS; t++)
		pthread_join(tid[t], NULL);

	for (int t = 0; t < THREADS; t++)
		for (int i = 0; i < PER; i++)
			if (objs[t][i].bucket >= 0)
				count[objs[t][i].bucket]++;
	for (int b = 0; b < NB; b++) {
		struct hlist_bl_node *pos;
		struct obj *o;
		int n = 0;

		assert(!hlist_bl_is_locked(&table[b]));
		hlist_bl_for_each_entry(o, pos, &table[b], node) {
			assert(o->bucket == b);
			n++;
		}
		assert(n == count[b]);
	}
	printf("ok: %d threads x %ld operations on %d bit-locked buckets\n",
	       THREADS, ops, NB);
	return 0;
}
