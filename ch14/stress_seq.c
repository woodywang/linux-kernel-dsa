// SPDX-License-Identifier: MIT
/*
 * stress_seq.c - readers against writers, for seqcount.h and latch.h.
 *
 *   cc -O2 -Wall -fsanitize=thread -pthread -o stress_seq stress_seq.c
 *   ./stress_seq [writes per writer] [seed]
 *
 * Part 1, seqlock: two writers (serialized by the seqlock's lock) store
 * a four-word record whose words are all derived from one number; reader
 * threads copy the record out and check that the four words still agree.
 * A torn copy -- some words from one write, some from another -- would
 * break the check.  Each reader also checks that the numbers it sees
 * never go backwards.
 *
 * Part 2, latch: one writer inserts and erases keys in a latched table
 * while readers copy the whole table out and look up keys.  Every copy
 * must be sorted, within capacity, and every value must carry its key.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include "latch.h"

#define READERS	2

static uint64_t seed;
static long writes;
static _Atomic int done;

static uint64_t rnd(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

static void fail(const char *what, uint64_t a, uint64_t b)
{
	fprintf(stderr, "%s: %#" PRIx64 " %#" PRIx64 "\n", what, a, b);
	exit(1);
}

/* ---- part 1: a seqlock-protected record ------------------------------ */

static struct seqlock sl;
static uint64_t version;		/* protected by sl.lock */
static _Atomic uint64_t rec[4];		/* protected by sl's counter */

static void *rec_writer(void *arg)
{
	(void)arg;
	for (long i = 0; i < writes; i++) {
		write_seqlock(&sl);
		uint64_t v = ++version;
		seq_store(&rec[0], v);
		seq_store(&rec[1], v * 0x9E3779B97F4A7C15ull);
		seq_store(&rec[2], ~v);
		seq_store(&rec[3], v ^ (v << 29));
		write_sequnlock(&sl);
	}
	return NULL;
}

static void *rec_reader(void *arg)
{
	uint64_t last = 0, r[4];

	(void)arg;
	while (!atomic_load(&done)) {
		unsigned seq;

		do {
			seq = read_seqbegin(&sl);
			for (int i = 0; i < 4; i++)
				r[i] = seq_load(&rec[i]);
		} while (read_seqretry(&sl, seq));
		if (r[1] != r[0] * 0x9E3779B97F4A7C15ull || r[2] != ~r[0] ||
		    r[3] != (r[0] ^ (r[0] << 29)))
			fail("torn record", r[0], r[2]);
		if (r[0] < last)
			fail("record went backwards", r[0], last);
		last = r[0];
	}
	return NULL;
}

/* ---- part 2: a latched table ------------------------------------------ */

static struct ltable lt;

static uint64_t value_of(uint64_t key, uint64_t gen)
{
	return gen << 32 | key;
}

static void *table_writer(void *arg)
{
	uint64_t s = seed;

	(void)arg;
	for (long i = 0; i < writes; i++) {
		uint64_t k = rnd(&s) % (2 * LT_CAP);

		if (rnd(&s) % 2 && atomic_load(&lt.copy[0].n) < LT_CAP)
			lt_insert(&lt, k, value_of(k, (uint64_t)i));
		else
			lt_erase(&lt, k);
	}
	return NULL;
}

static void *table_reader(void *arg)
{
	uint64_t s = seed ^ (uint64_t)(intptr_t)arg, key[LT_CAP], val[LT_CAP];

	while (!atomic_load(&done)) {
		int n = lt_snapshot(&lt, key, val);
		uint64_t k = rnd(&s) % (2 * LT_CAP), v;

		for (int i = 0; i < n; i++) {
			if (i && key[i] <= key[i - 1])
				fail("snapshot not sorted", key[i - 1], key[i]);
			if ((val[i] & 0xffffffff) != key[i])
				fail("value does not match key", key[i], val[i]);
		}
		if (lt_find(&lt, k, &v) && (v & 0xffffffff) != k)
			fail("lt_find() returned a foreign value", k, v);
	}
	return NULL;
}

static void run(void *(*writer)(void *), int nwriters,
		void *(*reader)(void *), const char *what)
{
	pthread_t w[2], r[READERS];

	atomic_store(&done, 0);
	for (int i = 0; i < READERS; i++)
		pthread_create(&r[i], NULL, reader, (void *)(intptr_t)(i + 1));
	for (int i = 0; i < nwriters; i++)
		pthread_create(&w[i], NULL, writer, NULL);
	for (int i = 0; i < nwriters; i++)
		pthread_join(w[i], NULL);
	atomic_store(&done, 1);
	for (int i = 0; i < READERS; i++)
		pthread_join(r[i], NULL);
	printf("stress_seq: %s: %d writer(s) x %ld writes, %d readers, "
	       "no torn reads\n", what, nwriters, writes, READERS);
}

int main(int argc, char **argv)
{
	writes = argc > 1 ? atol(argv[1]) : 200000;
	seed = (argc > 2 ? strtoull(argv[2], NULL, 0) : 1) *
	       0x9E3779B97F4A7C15ull + 1;
	seqlock_init(&sl);
	atomic_store(&rec[2], ~(uint64_t)0);	/* the record for version 0 */
	run(rec_writer, 2, rec_reader, "seqlock");
	run(table_writer, 1, table_reader, "latch  ");
	return 0;
}
