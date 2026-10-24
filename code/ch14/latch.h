// SPDX-License-Identifier: MIT
/*
 * latch.h - a latch sequence counter (the kernel's seqcount_latch_t in
 * include/linux/seqlock.h) and a small structure built on it: a sorted
 * table kept in two copies, the way include/linux/rbtree_latch.h keeps a
 * red-black tree in two copies.
 *
 * The writer changes copy 0 while the counter is odd, which sends readers
 * to copy 1, then makes the counter even, which sends them back to copy 0,
 * and changes copy 1.  One copy is always complete, so a reader never has
 * to wait for the writer, and may even run in the middle of a write on
 * the same thread (in the kernel: an NMI that interrupts the writer).  A
 * reader still checks the counter afterwards, because a slow reader can
 * be overtaken by two switches and find its copy being changed.
 *
 * Ordering is as in seqcount.h: the table's fields are atomics, written
 * with release and read with acquire.
 *
 * Deliberately left out: anything but fixed-size integer keys and values;
 * the RCU rules a pointer-based structure (like the latched rbtree) needs
 * for freeing nodes; and the kernel's KCSAN annotations, which are what
 * write_seqcount_latch_begin() / _end() add around the plain latch step.
 */
#ifndef LATCH_H
#define LATCH_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "seqcount.h"

struct seqcount_latch {
	_Atomic unsigned sequence;
};

static inline unsigned read_seqcount_latch(const struct seqcount_latch *s)
{
	return atomic_load_explicit(&s->sequence, memory_order_acquire);
}

static inline bool read_seqcount_latch_retry(const struct seqcount_latch *s,
					     unsigned start)
{
	return atomic_load_explicit(&s->sequence, memory_order_relaxed) != start;
}

#ifdef SEQ_NO_RETRY	/* deliberately broken (Exercise 14.4) */
#define read_seqcount_latch_retry(s, start)	((void)(s), (void)(start), false)
#endif

/* Send readers to the other copy. */
static inline void write_seqcount_latch(struct seqcount_latch *s)
{
	unsigned seq = atomic_load_explicit(&s->sequence, memory_order_relaxed);

	atomic_store_explicit(&s->sequence, seq + 1, memory_order_release);
}

/* ---- a latched sorted table ------------------------------------------ */

#define LT_CAP	64

struct table {
	_Atomic int n;
	_Atomic uint64_t key[LT_CAP];
	_Atomic uint64_t val[LT_CAP];
};

struct ltable {
	struct seqcount_latch seq;
	struct table copy[2];
};

/*
 * Called in the middle of changing one copy, to let tests play the part
 * of an interrupt that arrives while the writer is busy.
 */
static void (*lt_interrupt)(void);

static inline int t_search(struct table *t, uint64_t key, int n)
{
	int lo = 0, hi = n;		/* first index with key >= key */

	while (lo < hi) {
		int mid = (lo + hi) / 2;

		if (seq_load(&t->key[mid]) < key)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* Bound n, which a reader may see half-updated, to the array. */
static inline int t_count(struct table *t)
{
	int n = seq_load(&t->n);

	return n < 0 ? 0 : n > LT_CAP ? LT_CAP : n;
}

static inline void t_insert(struct table *t, uint64_t key, uint64_t val)
{
	int n = t_count(t), i = t_search(t, key, n);

	if (i < n && seq_load(&t->key[i]) == key) {
		seq_store(&t->val[i], val);
		return;
	}
	for (int j = n; j > i; j--) {		/* shift right, then insert */
		seq_store(&t->key[j], seq_load(&t->key[j - 1]));
		seq_store(&t->val[j], seq_load(&t->val[j - 1]));
		if (j == (n + i) / 2 + 1 && lt_interrupt)
			lt_interrupt();
	}
	seq_store(&t->key[i], key);
	seq_store(&t->val[i], val);
	seq_store(&t->n, n + 1);
}

static inline void t_erase(struct table *t, uint64_t key)
{
	int n = t_count(t), i = t_search(t, key, n);

	if (i == n || seq_load(&t->key[i]) != key)
		return;
	for (int j = i; j < n - 1; j++) {	/* shift left over it */
		seq_store(&t->key[j], seq_load(&t->key[j + 1]));
		seq_store(&t->val[j], seq_load(&t->val[j + 1]));
		if (j == (i + n) / 2 && lt_interrupt)
			lt_interrupt();
	}
	seq_store(&t->n, n - 1);
}

/* Writers must be serialized by the caller; the table must not be full. */
static inline void lt_insert(struct ltable *lt, uint64_t key, uint64_t val)
{
	write_seqcount_latch(&lt->seq);		/* readers -> copy 1 */
	t_insert(&lt->copy[0], key, val);
	write_seqcount_latch(&lt->seq);		/* readers -> copy 0 */
	t_insert(&lt->copy[1], key, val);
}

static inline void lt_erase(struct ltable *lt, uint64_t key)
{
	write_seqcount_latch(&lt->seq);
	t_erase(&lt->copy[0], key);
	write_seqcount_latch(&lt->seq);
	t_erase(&lt->copy[1], key);
}

/* Lockless lookup; never waits for the writer. */
static inline bool lt_find(struct ltable *lt, uint64_t key, uint64_t *val)
{
	unsigned seq;
	bool found;

	do {
		seq = read_seqcount_latch(&lt->seq);
		struct table *t = &lt->copy[seq & 1];
		int n = t_count(t), i = t_search(t, key, n);

		found = i < n && seq_load(&t->key[i]) == key;
		if (found)
			*val = seq_load(&t->val[i]);
	} while (read_seqcount_latch_retry(&lt->seq, seq));
	return found;
}

/* Copy the whole table out (for tests). */
static inline int lt_snapshot(struct ltable *lt, uint64_t *key, uint64_t *val)
{
	unsigned seq;
	int n;

	do {
		seq = read_seqcount_latch(&lt->seq);
		struct table *t = &lt->copy[seq & 1];

		n = t_count(t);
		for (int i = 0; i < n; i++) {
			key[i] = seq_load(&t->key[i]);
			val[i] = seq_load(&t->val[i]);
		}
	} while (read_seqcount_latch_retry(&lt->seq, seq));
	return n;
}

#endif /* LATCH_H */
