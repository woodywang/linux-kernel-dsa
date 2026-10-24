// SPDX-License-Identifier: MIT
/*
 * stress_llist.c - several producers, one consumer, one llist.
 *
 *   cc -O2 -Wall -fsanitize=thread -pthread -o stress_llist stress_llist.c
 *   ./stress_llist [items per producer] [seed]
 *
 * Each producer pushes its own items, numbered 0, 1, 2, ..., one at a
 * time or in small prelinked batches.  The consumer runs in three modes:
 *
 *   fifo  - only llist_del_all() + llist_reverse_order(); every producer's
 *           items must then arrive exactly in the order it pushed them;
 *   mixed - llist_del_first() and llist_del_all() at random; every item
 *           must arrive exactly once;
 *   kick  - the pattern of irq_work and delayed fput: a producer whose
 *           llist_add() returns true ("the list was empty") wakes the
 *           consumer, which takes everything with llist_del_all().  No
 *           item may be stranded, and no wakeup may find the list empty.
 */
#include <inttypes.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "container_of.h"
#include "llist.h"

#define PRODUCERS	3
#define MAXBATCH	4

struct item {
	struct llist_node node;
	int producer;
	long seq;
};

enum mode { FIFO, MIXED, KICK };

static struct llist_head head;
static struct item *items[PRODUCERS];
static long per_producer;
static uint64_t seed;
static enum mode mode;
static sem_t kick;

static uint64_t rnd(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

static void fail(const char *what, int p, long a, long b)
{
	fprintf(stderr, "producer %d: %s (%ld, %ld)\n", p, what, a, b);
	exit(1);
}

static void *producer(void *arg)
{
	int me = (int)(intptr_t)arg;
	uint64_t s = seed + (uint64_t)me;
	struct item *it = items[me];
	long next = 0;

	while (next < per_producer) {
		long n = (long)(rnd(&s) % MAXBATCH) + 1;
		bool was_empty;

		if (n > per_producer - next)
			n = per_producer - next;
		/* Link the batch newest first: next+n-1 -> ... -> next. */
		for (long i = next + n - 1; i > next; i--)
			it[i].node.next = &it[i - 1].node;
		was_empty = llist_add_batch(&it[next + n - 1].node,
					    &it[next].node, &head);
		next += n;
		if (mode == KICK && was_empty)
			sem_post(&kick);
	}
	return NULL;
}

static long *expect;		/* FIFO: next seq per producer */
static unsigned char *seen;	/* MIXED/KICK: one byte per item */

static void consume(struct item *it)
{
	if (mode == FIFO) {
		if (it->seq != expect[it->producer])
			fail("out of order", it->producer, it->seq,
			     expect[it->producer]);
		expect[it->producer]++;
	} else {
		unsigned char *b = &seen[it->producer * per_producer + it->seq];

		if (*b)
			fail("seen twice", it->producer, it->seq, 0);
		*b = 1;
	}
}

static long take_all(void)
{
	struct llist_node *n, *list = llist_reverse_order(llist_del_all(&head));
	long got = 0;

	llist_for_each(n, list) {
		consume(container_of(n, struct item, node));
		got++;
	}
	return got;
}

static void run(enum mode m, const char *name)
{
	pthread_t p[PRODUCERS];
	long total = PRODUCERS * per_producer, got = 0, wakeups = 0, pops = 0;
	uint64_t s = seed ^ 0xabcdef;

	mode = m;
	init_llist_head(&head);
	memset(expect, 0, PRODUCERS * sizeof(*expect));
	memset(seen, 0, (size_t)total);
	sem_init(&kick, 0, 0);
	for (int i = 0; i < PRODUCERS; i++)
		pthread_create(&p[i], NULL, producer, (void *)(intptr_t)i);

	while (got < total) {
		if (m == KICK) {
			long n;

			sem_wait(&kick);
			wakeups++;
			n = take_all();
			if (n == 0)
				fail("woken up for an empty list", -1, wakeups, 0);
			got += n;
		} else if (m == MIXED && rnd(&s) % 4) {
			struct llist_node *n = llist_del_first(&head);

			if (n) {
				consume(container_of(n, struct item, node));
				got++;
				pops++;
			}
		} else {
			got += take_all();
		}
	}
	for (int i = 0; i < PRODUCERS; i++)
		pthread_join(p[i], NULL);
	if (!llist_empty(&head))
		fail("list not empty at the end", -1, 0, 0);
	for (long i = 0; m != FIFO && i < total; i++)
		if (!seen[i])
			fail("item lost", (int)(i / per_producer),
			     i % per_producer, 0);
	sem_destroy(&kick);
	printf("stress_llist: %-5s %d producers x %ld items, all received",
	       name, PRODUCERS, per_producer);
	if (m == FIFO)
		printf(" in order\n");
	else if (m == MIXED)
		printf(" exactly once\n");
	else
		printf(", no empty wakeup\n");
	(void)pops;
}

int main(int argc, char **argv)
{
	per_producer = argc > 1 ? atol(argv[1]) : 200000;
	seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	seed = seed * 0x9E3779B97F4A7C15ull + 1;
	expect = calloc(PRODUCERS, sizeof(*expect));
	seen = calloc((size_t)(PRODUCERS * per_producer), 1);
	for (int p = 0; p < PRODUCERS; p++) {
		items[p] = calloc((size_t)per_producer, sizeof(struct item));
		for (long i = 0; i < per_producer; i++)
			items[p][i] = (struct item){ .producer = p, .seq = i };
	}
	run(FIFO, "fifo");
	run(MIXED, "mixed");
	run(KICK, "kick");
	for (int p = 0; p < PRODUCERS; p++)
		free(items[p]);
	free(expect);
	free(seen);
	return 0;
}
