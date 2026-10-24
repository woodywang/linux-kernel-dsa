// SPDX-License-Identifier: MIT
/*
 * stress_spsc.c - one producer thread, one consumer thread, one small ring.
 *
 *   cc -O2 -Wall -fsanitize=thread -pthread -o stress_spsc stress_spsc.c
 *   ./stress_spsc [elements] [seed]
 *
 * The producer writes elements {seq, check} with consecutive sequence
 * numbers, mixing single puts and batches of random size; the consumer
 * reads them the same way and checks that every element arrives once, in
 * order, and intact.  The ring has only 64 slots and its indices start
 * just below UINT_MAX, so the run spends most of its time wrapping, full,
 * or empty.  Built with -fsanitize=thread, ThreadSanitizer also checks
 * that every access to the data array is ordered by the acquire/release
 * pairs in spsc.h.  Built with -DSPSC_RELAXED as well, it must report a
 * data race (Exercise 5.1).
 */
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include "spsc.h"

#define SLOTS	64
#define BATCH	16

struct elem {
	uint64_t seq;
	uint64_t check;
};

static struct spsc ring;
static uint64_t total;
static uint64_t seed;

static uint64_t check_of(uint64_t seq)
{
	return seq * 0x9E3779B97F4A7C15ull ^ 0x5555;
}

static unsigned int rnd(uint64_t *s, unsigned int n)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return (unsigned int)(*s % n) + 1;
}

static void *producer(void *arg)
{
	struct elem b[BATCH];
	uint64_t s = seed, next = 0;

	(void)arg;
	while (next < total) {
		unsigned int n = rnd(&s, BATCH), done;

		if (n > total - next)
			n = (unsigned int)(total - next);
		for (unsigned int i = 0; i < n; i++)
			b[i] = (struct elem){ next + i, check_of(next + i) };
		if (n == 1)
			done = spsc_put(&ring, &b[0]);
		else
			done = spsc_in(&ring, b, n);
		next += done;
		if (!done)
			sched_yield();		/* full: let the consumer run */
	}
	return NULL;
}

static void *consumer(void *arg)
{
	struct elem b[BATCH];
	uint64_t s = seed ^ 0xabcdef, next = 0;

	(void)arg;
	while (next < total) {
		unsigned int n = rnd(&s, BATCH), got;

		got = n == 1 ? spsc_get(&ring, &b[0]) : spsc_out(&ring, b, n);
		for (unsigned int i = 0; i < got; i++, next++) {
			if (b[i].seq != next || b[i].check != check_of(next)) {
				fprintf(stderr, "element %" PRIu64 ": got seq %"
					PRIu64 " check %#" PRIx64 "\n",
					next, b[i].seq, b[i].check);
				exit(1);
			}
		}
		if (!got)
			sched_yield();		/* empty: let the producer run */
	}
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t p, c;

	total = argc > 1 ? strtoull(argv[1], NULL, 0) : 1000000;
	seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	seed = seed * 0x9E3779B97F4A7C15ull + 1;
	if (spsc_init_at(&ring, SLOTS, sizeof(struct elem), UINT_MAX - 100))
		return 1;
	pthread_create(&c, NULL, consumer, NULL);
	pthread_create(&p, NULL, producer, NULL);
	pthread_join(p, NULL);
	pthread_join(c, NULL);
	printf("stress_spsc: %" PRIu64 " elements through a %u-slot ring, "
	       "in order and intact\n", total, spsc_size(&ring));
	spsc_free(&ring);
	return 0;
}
