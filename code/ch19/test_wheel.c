// SPDX-License-Identifier: MIT
/*
 * test_wheel.c - randomized tests for wheel.c.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -o test_wheel test_wheel.c wheel.c
 *   ./test_wheel [steps] [seed]
 *
 * A model keeps, for every timer, whether it is armed and the window in
 * which it may fire, computed from the level rule alone: a timer armed
 * delta ticks ahead lands in level n, the first whose range covers delta,
 * and must fire after its expiry and at most LVL_GRAN(n) ticks after it.
 * The test checks that
 *   - every timer fires inside its window, and exactly at its rounded-up
 *     bucket time; cancelled timers never fire;
 *   - after running up to now, no armed timer's window has passed;
 *   - the cached next_expiry is never later than the first bucket expiry
 *     (a safe lower bound), and wheel_next_expiry() returns it exactly;
 *   - the pending bitmap has a bit set exactly for non-empty buckets.
 * Callbacks re-arm themselves, re-arm other timers and cancel others, and
 * the clock starts 300 seconds before the tick counter wraps (the kernel
 * starts its 32-bit jiffies that close to wrapping, to expose wrap bugs).
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "wheel.h"

#define NT	1024

struct ttimer {
	struct wtimer t;
	bool armed;			/* the model's view */
	unsigned long lo, hi;		/* must fire in (lo, hi] */
	unsigned long tick;		/* the exact bucket time */
};

static struct wheel w;
static struct ttimer tt[NT];
static unsigned long now, nfired, ncross, ncutoff, nshortcut;
static bool draining;
static unsigned long long rng_state;

static unsigned long rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (unsigned long)rng_state;
}

/* The level rule, written independently of calc_wheel_index(). */
static unsigned int level_of(unsigned long delta)
{
	unsigned long start = LVL_SIZE - 1;	/* level 1 starts here */
	unsigned int lvl;

	for (lvl = 0; lvl < LVL_DEPTH - 1; lvl++, start *= LVL_CLK_DIV)
		if (delta < start)
			break;
	return lvl;
}

/* A random expiry relative to now: mostly short, some long, a few odd. */
static unsigned long rand_expiry(void)
{
	unsigned long r = rnd() % 100;

	if (r < 40)
		return now + rnd() % 100;
	if (r < 70) {
		unsigned long bits = rnd() % 21;

		return now + rnd() % (1UL << bits);
	}
	if (r < 85)
		return now + rnd() % WHEEL_TIMEOUT_CUTOFF;
	if (r < 87)
		return now + WHEEL_TIMEOUT_CUTOFF + rnd() % (1UL << 32);
	if (r < 92)
		return now - 1 - rnd() % 50;		/* already in the past */
	return now + rnd() % 8;
}

/* Arm through the wheel and record the window the model allows. */
static void arm(struct ttimer *x, unsigned long expires)
{
	bool same = x->armed && x->t.idx != NO_BUCKET;
	unsigned int old_idx = x->t.idx;
	unsigned long clk, delta;
	int ret;

	if (x->armed && expires == x->t.expires) {	/* nothing changes */
		assert(wheel_mod(&w, &x->t, expires, now) == 1);
		return;
	}
	ret = wheel_mod(&w, &x->t, expires, now);
	clk = w.clk;
	delta = expires - clk;
	assert(ret == (int)x->armed);
	if (same && x->t.idx == old_idx)
		nshortcut++;
	x->armed = true;
	if ((long)delta < 0) {			/* due: the next tick */
		x->lo = clk - 1;
		x->hi = clk;
		x->tick = clk;
	} else if (delta >= WHEEL_TIMEOUT_CUTOFF) {	/* clamped: early */
		unsigned long e = clk + WHEEL_TIMEOUT_MAX;
		unsigned int s = LVL_SHIFT(LVL_DEPTH - 1);

		x->lo = e;
		x->hi = e + LVL_GRAN(LVL_DEPTH - 1);
		x->tick = ((e >> s) + 1) << s;
		ncutoff++;
	} else {
		unsigned int s = LVL_SHIFT(level_of(delta));

		x->lo = expires;
		x->hi = expires + (1UL << s);
		x->tick = ((expires >> s) + 1) << s;
	}
}

static void callback(struct wtimer *t, unsigned long tick)
{
	struct ttimer *x = container_of(t, struct ttimer, t);
	unsigned long r = rnd() % 100;

	assert(x->armed);			/* cancelled timers never fire */
	assert(!wtimer_pending(t));
	assert(time_after(tick, x->lo));	/* not early */
	assert(time_before_eq(tick, x->hi));	/* not too late */
	assert(tick == x->tick);
	assert(time_before_eq(tick, now));
	x->armed = false;
	nfired++;

	if (draining)
		return;
	if (r < 30) {
		arm(x, rand_expiry());		/* periodic: re-arm itself */
	} else if (r < 40) {
		struct ttimer *y = &tt[rnd() % NT];

		arm(y, rand_expiry());		/* re-arm another */
		ncross++;
	} else if (r < 50) {
		struct ttimer *y = &tt[rnd() % NT];

		assert(wheel_cancel(&w, &y->t) == (int)y->armed);
		y->armed = false;
	}
}

static unsigned long true_next(void)
{
	unsigned long best = w.clk + NEXT_MAX_DELTA;

	for (int i = 0; i < NT; i++)
		if (tt[i].armed && time_before(tt[i].tick, best))
			best = tt[i].tick;
	return best;
}

static void check_all(void)
{
	static unsigned long count[WHEEL_SIZE];

	for (unsigned long b = 0; b < WHEEL_SIZE; b++)
		count[b] = 0;
	for (int i = 0; i < NT; i++) {
		struct ttimer *x = &tt[i];

		assert(wtimer_pending(&x->t) == x->armed);
		if (!x->armed)
			continue;
		assert(time_after_eq(x->hi, now));	/* nothing overdue */
		assert(time_after_eq(x->tick, w.clk));
		count[x->t.idx]++;
	}
	for (unsigned long b = 0; b < WHEEL_SIZE; b++)
		assert(test_bit(b, w.pending_map) == (count[b] > 0));
}

static void check_next(void)
{
	unsigned long want = true_next();

	assert(time_before_eq(w.next_expiry, want));	/* a lower bound */
	assert(wheel_next_expiry(&w) == want);			/* exact */
}

/*
 * A directed case the random test almost never hits: timers a and b
 * expire in the same tick; a's callback re-arms b one lap of level 0
 * (64 buckets) later, into the very bucket b was just taken from.  b must
 * then fire at its new expiry, not with a.
 */
static struct wtimer sa, sb;
static unsigned long sb_tick;

static void sa_fn(struct wtimer *t, unsigned long tick)
{
	(void)t;
	wheel_mod(&w, &sb, tick + 63, tick);
}

static void sb_fn(struct wtimer *t, unsigned long tick)
{
	(void)t;
	sb_tick = tick;
}

static void sibling_rearm(void)
{
	unsigned long t0 = 1000;

	wheel_init(&w, t0);
	wtimer_init(&sa, sa_fn);
	wtimer_init(&sb, sb_fn);
	wheel_mod(&w, &sb, t0 + 10, t0);
	wheel_mod(&w, &sa, t0 + 10, t0);	/* runs first: added last */
	for (unsigned long t = t0; t < t0 + 200 && !sb_tick; t++)
		wheel_run(&w, t);
	if (!time_after(sb_tick, sb.expires)) {
		printf("sibling re-arm: b expires at %lu but fired at %lu\n",
		       sb.expires, sb_tick);
		exit(1);
	}
}

int main(int argc, char **argv)
{
	unsigned long steps = argc > 1 ? strtoul(argv[1], NULL, 0) : 200000;
	unsigned long seed = argc > 2 ? strtoul(argv[2], NULL, 0) : 1;

	sibling_rearm();
	rng_state = 0x9e3779b97f4a7c15ULL * (seed + 1);
	now = -300UL * HZ;			/* cf. the kernel's INITIAL_JIFFIES */
	wheel_init(&w, now);
	for (int i = 0; i < NT; i++)
		wtimer_init(&tt[i].t, callback);

	for (unsigned long step = 0; step < steps; step++) {
		unsigned long r = rnd() % 100;
		struct ttimer *x = &tt[rnd() % NT];

		if (r < 45) {
			arm(x, rand_expiry());
		} else if (r < 50 && x->armed) {
			arm(x, x->t.expires + rnd() % 3);	/* nudge */
		} else if (r < 80) {
			assert(wheel_cancel(&w, &x->t) == (int)x->armed);
			x->armed = false;
		} else {
			unsigned long q = rnd() % 100;

			now += q < 80 ? rnd() % 8 : q < 98 ? rnd() % 5000 :
			       rnd() % (1UL << 22);
			wheel_run(&w, now);
		}
		check_next();
		if (step % 16 == 0)
			check_all();
	}

	/* Drain: everything still armed must fire inside its window. */
	draining = true;
	for (int rounds = 0; rounds < 4096; rounds++) {
		bool any = false;

		for (int i = 0; i < NT; i++)
			any |= tt[i].armed;
		if (!any)
			break;
		now += 1UL << 20;
		wheel_run(&w, now);
		check_all();
		check_next();
	}
	for (int i = 0; i < NT; i++)
		assert(!tt[i].armed);
	printf("test_wheel HZ=%d: %lu steps, %lu fired (%lu cross re-arms, "
	       "%lu clamped, %lu same-bucket), ok\n", HZ, steps, nfired, ncross,
	       ncutoff, nshortcut);
	return 0;
}
