// SPDX-License-Identifier: MIT
/*
 * wheel.h - a teaching version of the kernel's non-cascading timer wheel
 * (kernel/time/timer.c, Linux 4.8 and later).
 *
 * Time is counted in ticks ("jiffies").  The wheel has LVL_DEPTH levels of
 * 64 buckets; level n has a granularity of 8^n ticks.  A timer is put in
 * the level whose range covers its timeout and stays in that bucket until
 * it expires or is cancelled: there is no cascading.  Its expiry is rounded
 * *up* to the bucket's granularity, so it fires late by at most one granule
 * of its level and never early.  One pending bit per bucket lets the wheel
 * find the next expiring bucket with a bitmap search (Chapter 4).
 *
 * What it shows:   the level/granularity arithmetic, bucketing with
 *                  round-up, add / modify (with the same-bucket shortcut) /
 *                  cancel, expiry by levels, the lazy next-expiry
 *                  recalculation, and the next-expiry search.
 * What it leaves out on purpose: locking and per-CPU bases, deferrable and
 *                  pinned timers, timer migration, the "running timer"
 *                  bookkeeping that lets a caller wait for a callback, NOHZ
 *                  idle handling, and debug objects.  Callbacks get the
 *                  tick whose bucket expired as an argument (the current
 *                  time, passed to wheel_run(), may be later).
 * What it adds:    timers taken out of their bucket for expiry are marked
 *                  (idx = NO_BUCKET), so that re-arming one from another
 *                  timer's callback cannot take the same-bucket shortcut
 *                  and fire it early.  The kernel has no such mark.
 */
#ifndef CH19_WHEEL_H
#define CH19_WHEEL_H

#include <stdbool.h>
#include "bitmap.h"
#include "hlist.h"

#ifndef HZ
#define HZ		1000		/* ticks per second */
#endif

/* Each level's clock runs 8 times slower than the one below. */
#define LVL_CLK_SHIFT	3
#define LVL_CLK_DIV	(1UL << LVL_CLK_SHIFT)
#define LVL_CLK_MASK	(LVL_CLK_DIV - 1)
#define LVL_SHIFT(n)	((n) * LVL_CLK_SHIFT)
#define LVL_GRAN(n)	(1UL << LVL_SHIFT(n))

/* 64 buckets per level: one word of the pending bitmap per level. */
#define LVL_BITS	6
#define LVL_SIZE	(1UL << LVL_BITS)
#define LVL_MASK	(LVL_SIZE - 1)
#define LVL_OFFS(n)	((n) * LVL_SIZE)

/* Smallest delta that goes to level n (n >= 1). */
#define LVL_START(n)	((LVL_SIZE - 1) << (((n) - 1) * LVL_CLK_SHIFT))

#if HZ > 100
#define LVL_DEPTH	9
#else
#define LVL_DEPTH	8
#endif

#define WHEEL_SIZE		(LVL_SIZE * LVL_DEPTH)
#define WHEEL_TIMEOUT_CUTOFF	(LVL_START(LVL_DEPTH))
#define WHEEL_TIMEOUT_MAX	(WHEEL_TIMEOUT_CUTOFF - LVL_GRAN(LVL_DEPTH - 1))
#define NEXT_MAX_DELTA		((1UL << 30) - 1)	/* "nothing pending" */
#define NO_BUCKET		((unsigned int)WHEEL_SIZE)	/* being expired */

/* Tick counts wrap around; compare them by the sign of the difference. */
#define time_after(a, b)	((long)((b) - (a)) < 0)
#define time_before(a, b)	time_after(b, a)
#define time_after_eq(a, b)	((long)((a) - (b)) >= 0)
#define time_before_eq(a, b)	time_after_eq(b, a)

struct wtimer {
	struct hlist_node entry;	/* in a bucket while pending */
	unsigned long expires;		/* requested expiry, in ticks */
	unsigned int idx;		/* bucket, or NO_BUCKET while expiring */
	void (*fn)(struct wtimer *t, unsigned long tick);	/* tick it expired at */
};

struct wheel {
	unsigned long clk;		/* the next tick to process */
	unsigned long next_expiry;	/* first bucket expiry, or earlier */
	bool next_expiry_recalc;	/* next_expiry may be too early */
	bool timers_pending;
	DECLARE_BITMAP(pending_map, WHEEL_SIZE);
	struct hlist_head vectors[WHEEL_SIZE];
};

void wheel_init(struct wheel *w, unsigned long now);
void wtimer_init(struct wtimer *t, void (*fn)(struct wtimer *, unsigned long));

static inline bool wtimer_pending(const struct wtimer *t)
{
	return !hlist_unhashed(&t->entry);
}

/* Arm or re-arm t; returns 1 if it was pending before. */
int wheel_mod(struct wheel *w, struct wtimer *t, unsigned long expires,
	      unsigned long now);
/* Disarm t; returns 1 if it was pending. */
int wheel_cancel(struct wheel *w, struct wtimer *t);
/* Run every timer due at or before now; returns how many fired. */
unsigned long wheel_run(struct wheel *w, unsigned long now);
/* When the first pending bucket expires (clk + NEXT_MAX_DELTA if none). */
unsigned long wheel_next_expiry(struct wheel *w);

/* Exposed for the demo and the tests. */
unsigned int calc_wheel_index(unsigned long expires, unsigned long clk,
			      unsigned long *bucket_expiry);

#endif /* CH19_WHEEL_H */
