// SPDX-License-Identifier: MIT
/*
 * test_seq.c - single-threaded tests of seqcount.h and latch.h.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -o test_seq test_seq.c
 *   ./test_seq [steps] [seed]
 *
 * Seqcount: a read section that overlaps a write section must be told to
 * retry, and one that does not overlap must not be.
 *
 * Latch: random inserts and erases on a latched table, checked against a
 * plain sorted array.  Every change calls lt_interrupt() in the middle of
 * rewriting each copy; the "interrupt" reads the whole table and must see
 * exactly the state before the change (while copy 0 is being rewritten)
 * or exactly the state after it (while copy 1 is), and must never need
 * to retry.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "latch.h"

static uint64_t rng;

static uint64_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

static void fail(const char *what, long step)
{
	fprintf(stderr, "step %ld: %s\n", step, what);
	exit(1);
}

struct model {
	int n;
	uint64_t key[LT_CAP], val[LT_CAP];
};

static struct ltable lt;
static struct model before, after;
static long step, interrupts;

static void model_insert(struct model *m, uint64_t key, uint64_t val)
{
	int i = 0;

	while (i < m->n && m->key[i] < key)
		i++;
	if (i < m->n && m->key[i] == key) {
		m->val[i] = val;
		return;
	}
	memmove(m->key + i + 1, m->key + i, (m->n - i) * sizeof(uint64_t));
	memmove(m->val + i + 1, m->val + i, (m->n - i) * sizeof(uint64_t));
	m->key[i] = key;
	m->val[i] = val;
	m->n++;
}

static void model_erase(struct model *m, uint64_t key)
{
	for (int i = 0; i < m->n; i++)
		if (m->key[i] == key) {
			memmove(m->key + i, m->key + i + 1,
				(m->n - i - 1) * sizeof(uint64_t));
			memmove(m->val + i, m->val + i + 1,
				(m->n - i - 1) * sizeof(uint64_t));
			m->n--;
			return;
		}
}

static bool same(const struct model *m, int n, const uint64_t *key,
		 const uint64_t *val)
{
	return n == m->n && !memcmp(key, m->key, n * sizeof(uint64_t)) &&
	       !memcmp(val, m->val, n * sizeof(uint64_t));
}

/* Plays an NMI: runs a reader in the middle of the writer. */
static void interrupt(void)
{
	uint64_t key[LT_CAP], val[LT_CAP];
	unsigned seq = read_seqcount_latch(&lt.seq);
	const struct model *expect = seq & 1 ? &before : &after;
	int n = lt_snapshot(&lt, key, val);

	if (read_seqcount_latch_retry(&lt.seq, seq))
		fail("latch reader had to retry", step);
	if (!same(expect, n, key, val))
		fail(seq & 1 ? "interrupt did not see the old state"
			     : "interrupt did not see the new state", step);
	interrupts++;
}

static void test_seqcount(void)
{
	struct seqcount s;
	unsigned r;

	seqcount_init(&s);
	r = read_seqcount_begin(&s);
	if (read_seqcount_retry(&s, r))
		fail("retry without a writer", 0);
	write_seqcount_begin(&s);
	if (!(raw_read_seqcount(&s) & 1))
		fail("counter not odd inside a write", 0);
	write_seqcount_end(&s);
	if (!read_seqcount_retry(&s, r))
		fail("no retry after an overlapping write", 0);
	r = read_seqcount_begin(&s);
	if (r != 2 || read_seqcount_retry(&s, r))
		fail("bad counter after one write", 0);
}

int main(int argc, char **argv)
{
	long steps = argc > 1 ? atol(argv[1]) : 100000;
	uint64_t key[LT_CAP], val[LT_CAP];

	rng = (argc > 2 ? strtoull(argv[2], NULL, 0) : 1) *
	      0x9E3779B97F4A7C15ull + 1;
	test_seqcount();

	memset(&lt, 0, sizeof(lt));
	lt_interrupt = interrupt;
	for (step = 0; step < steps; step++) {
		uint64_t k = rnd() % 100, v = rnd();

		before = after;
		if (rnd() % 2 && after.n < LT_CAP) {
			model_insert(&after, k, v);
			lt_insert(&lt, k, v);
		} else {
			model_erase(&after, k);
			lt_erase(&lt, k);
		}
		if (!same(&after, lt_snapshot(&lt, key, val), key, val))
			fail("table differs from the model", step);
		for (int i = 0; i < 4; i++) {
			uint64_t q = rnd() % 100, got = 0;
			bool found = lt_find(&lt, q, &got), want = false;

			for (int j = 0; j < after.n; j++)
				if (after.key[j] == q) {
					want = true;
					if (got != after.val[j])
						fail("lt_find() wrong value", step);
				}
			if (found != want)
				fail("lt_find() wrong presence", step);
		}
	}
	printf("test_seq: seqcount ok; latch: %ld changes, %ld interrupted "
	       "reads, none retried\n", steps, interrupts);
	return 0;
}
