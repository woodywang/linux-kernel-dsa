// SPDX-License-Identifier: MIT
/*
 * heapq.h - a timer queue on a binary min-heap: a model of the hrtimer
 * queue (kernel/time/hrtimer.c) and the comparison point for the wheel.
 *
 * Times are 64-bit nanoseconds.  A timer may fire anywhere in [soft, hard]
 * (hard = soft + slack), and the queue is ordered by the hard expiry, as
 * the kernel's is.  Running the queue at time now fires timers from the
 * front while the front timer's soft expiry has passed, and stops at the
 * first one that may still wait.  Whoever owns the hardware programs it
 * for the front timer's hard expiry; hq_add() and hq_mod() return true
 * when the front timer changed, which is when that is needed.
 *
 * The kernel keeps these timers in a red-black tree with a cached leftmost
 * node (Chapter 6), which removes any timer through its embedded node.  A
 * heap can do the same only if every timer remembers its slot, which is
 * what idx is for.
 *
 * What it leaves out on purpose: clock bases and their offsets, per-CPU
 * queues, soft-interrupt timers, locking, and the callback's return value
 * (a callback that wants to run again simply re-adds its timer).
 */
#ifndef CH19_HEAPQ_H
#define CH19_HEAPQ_H

#include <stdbool.h>
#include <stddef.h>

typedef unsigned long long ktime;	/* nanoseconds */

#define HQ_NONE	((size_t)-1)		/* idx of a timer not in a queue */

struct htimer {
	ktime soft, hard;		/* fire anywhere in [soft, hard] */
	size_t idx;			/* slot in the heap, or HQ_NONE */
	void (*fn)(struct htimer *t, ktime now);
};

struct heapq {
	size_t nr, size;		/* timers queued, capacity of h[] */
	struct htimer **h;		/* the caller's array */
	unsigned long moves;		/* element moves, for the demo */
};

static inline void hq_init(struct heapq *q, struct htimer **array, size_t size)
{
	q->nr = 0;
	q->size = size;
	q->h = array;
	q->moves = 0;
}

static inline void htimer_init(struct htimer *t, void (*fn)(struct htimer *, ktime))
{
	t->soft = t->hard = 0;
	t->idx = HQ_NONE;
	t->fn = fn;
}

static inline bool htimer_queued(const struct htimer *t)
{
	return t->idx != HQ_NONE;
}

static inline struct htimer *hq_first(const struct heapq *q)
{
	return q->nr ? q->h[0] : NULL;
}

static inline void hq_set(struct heapq *q, size_t i, struct htimer *t)
{
	q->h[i] = t;
	t->idx = i;
	q->moves++;
}

static inline void hq_sift_up(struct heapq *q, size_t i)
{
	struct htimer *t = q->h[i];

	while (i > 0) {
		size_t parent = (i - 1) / 2;

		if (q->h[parent]->hard <= t->hard)
			break;
		hq_set(q, i, q->h[parent]);
		i = parent;
	}
	hq_set(q, i, t);
}

static inline void hq_sift_down(struct heapq *q, size_t i)
{
	struct htimer *t = q->h[i];

	for (;;) {
		size_t c = 2 * i + 1;

		if (c >= q->nr)
			break;
		if (c + 1 < q->nr && q->h[c + 1]->hard < q->h[c]->hard)
			c++;
		if (t->hard <= q->h[c]->hard)
			break;
		hq_set(q, i, q->h[c]);
		i = c;
	}
	hq_set(q, i, t);
}

/* Queue t (not queued, room in h[]); true if it became the first timer. */
static inline bool hq_add(struct heapq *q, struct htimer *t, ktime soft, ktime slack)
{
	t->soft = soft;
	t->hard = soft + slack;
	q->h[q->nr++] = t;
	hq_sift_up(q, q->nr - 1);
	return q->h[0] == t;
}

/* Remove t from anywhere in the queue; false if it was not queued. */
static inline bool hq_del(struct heapq *q, struct htimer *t)
{
	size_t i = t->idx;
	struct htimer *last;

	if (i == HQ_NONE)
		return false;
	t->idx = HQ_NONE;
	last = q->h[--q->nr];
	if (last != t) {		/* fill the hole with the last timer */
		q->h[i] = last;
		last->idx = i;
		hq_sift_up(q, i);
		hq_sift_down(q, last->idx);
	}
	return true;
}

/* Change a queued timer's expiry in place; true if the first timer changed. */
static inline bool hq_mod(struct heapq *q, struct htimer *t, ktime soft, ktime slack)
{
	struct htimer *first = hq_first(q);

	t->soft = soft;
	t->hard = soft + slack;
	hq_sift_up(q, t->idx);
	hq_sift_down(q, t->idx);
	return first != q->h[0] || first == t;
}

/*
 * Fire timers from the front while their soft expiry has passed.  A timer
 * behind a front timer that may still wait is left alone even if its own
 * soft expiry has passed: the front timer will cause a wakeup anyway.
 */
static inline unsigned long hq_run(struct heapq *q, ktime now)
{
	struct htimer *t;
	unsigned long n = 0;

	while ((t = hq_first(q)) && t->soft <= now) {
		hq_del(q, t);
		t->fn(t, now);		/* may re-add t or touch others */
		n++;
	}
	return n;
}

#endif /* CH19_HEAPQ_H */
