// SPDX-License-Identifier: MIT
/*
 * wheel_demo.c - the numbers quoted in Chapter 19.
 *
 *   cc -O2 -Wall -o wheel_demo wheel_demo.c wheel.c
 *   ./wheel_demo
 *
 * 1. The wheel's geometry for this HZ: each level's granularity and the
 *    timeouts it holds.
 * 2. Where some typical timeouts land and how late they fire.
 * 3. The lazy next-expiry recalculation after a cancel.
 * 4. A workload of 100,000 timeouts, most cancelled: how late the wheel
 *    fires the rest, and how many element moves the same work costs a
 *    binary heap.  (Counts, not timings: Exercise 19.3 measures time.)
 */
#include <stdio.h>
#include "heapq.h"
#include "wheel.h"

static const char *fmt(char *buf, double ms)
{
	if (ms < 1000)
		sprintf(buf, "%.0f ms", ms);
	else if (ms < 60e3)
		sprintf(buf, "%.1f s", ms / 1e3);
	else if (ms < 3600e3)
		sprintf(buf, "%.1f min", ms / 60e3);
	else if (ms < 86400e3)
		sprintf(buf, "%.1f h", ms / 3600e3);
	else
		sprintf(buf, "%.1f d", ms / 86400e3);
	return buf;
}

#define MS(ticks)	((double)(ticks) * 1000 / HZ)

static void geometry(void)
{
	char a[32], b[32], c[32];

	printf("HZ=%d: %d levels x %lu buckets = %lu lists, %lu bitmap words\n",
	       HZ, LVL_DEPTH, LVL_SIZE, WHEEL_SIZE, WHEEL_SIZE / 64);
	printf("level  granularity  holds timeouts of\n");
	for (unsigned int lvl = 0; lvl < LVL_DEPTH; lvl++) {
		unsigned long lo = lvl ? LVL_START(lvl) : 0;
		unsigned long hi = (lvl < LVL_DEPTH - 1 ? LVL_START(lvl + 1) :
				    WHEEL_TIMEOUT_CUTOFF) - 1;

		printf("  %u   %10s     %8s to %s\n", lvl,
		       fmt(a, MS(LVL_GRAN(lvl))), fmt(b, MS(lo)), fmt(c, MS(hi)));
	}
}

static void placement(void)
{
	static const unsigned long ms[] = { 10, 100, 1000, 30000, 300000, 3600000 };
	unsigned long clk = 12345;
	char a[32], b[32], c[32];

	printf("\narmed at tick %lu:\n", clk);
	printf("  timeout  level  bucket  fires after  late by\n");
	for (unsigned int i = 0; i < sizeof(ms) / sizeof(ms[0]); i++) {
		unsigned long delta = ms[i] * HZ / 1000, bexp;
		unsigned int idx = calc_wheel_index(clk + delta, clk, &bexp);

		printf("  %7s    %u     %3u   %10s  %8s\n", fmt(a, ms[i]),
		       idx / (unsigned int)LVL_SIZE, idx, fmt(b, MS(bexp - clk)),
		       fmt(c, MS(bexp - clk - delta)));
	}
}

static void nop(struct wtimer *t, unsigned long tick)
{
	(void)t;
	(void)tick;
}

static void lazy_next(void)
{
	static struct wheel w;
	struct wtimer t[3];
	unsigned long clk = 12345;

	wheel_init(&w, clk);
	for (int i = 0; i < 3; i++)
		wtimer_init(&t[i], nop);
	wheel_mod(&w, &t[0], clk + 10, clk);
	wheel_mod(&w, &t[1], clk + 1000, clk);
	wheel_mod(&w, &t[2], clk + 3600000, clk);
	printf("\nnext expiry with 10 ms, 1 s, 1 h pending: +%lu\n",
	       wheel_next_expiry(&w) - clk);
	wheel_cancel(&w, &t[0]);
	printf("cancel the 10 ms timer: cached +%lu (%s)\n", w.next_expiry - clk,
	       w.next_expiry_recalc ? "stale, may be early" : "exact");
	printf("recomputed when asked:  +%lu\n", wheel_next_expiry(&w) - clk);
}

/* ------------------------------------------------------------------ */

#define N	100000

static struct wtimer wt[N];
static struct htimer ht[N];
static struct htimer *slots[N];
static unsigned long timeout[N], fired, fired0, late0;
static double late_sum, late_max;
static unsigned long long rng_state = 42;

static unsigned long rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (unsigned long)rng_state;
}

static void wfire(struct wtimer *t, unsigned long tick)
{
	unsigned long i = t - wt;
	double late = (double)(tick - t->expires) / timeout[i];

	if (timeout[i] < LVL_START(1)) {	/* level 0 */
		fired0++;
		late0 += tick - t->expires;
		return;
	}
	late_sum += late;
	if (late > late_max)
		late_max = late;
	fired++;
}

static void hfire(struct htimer *t, ktime now)
{
	(void)t;
	(void)now;
}

static void workload(void)
{
	static struct wheel w;
	struct heapq q;
	unsigned long order[N], ncancel = N / 100 * 95;
	unsigned long m0, adds, cancels;

	/* Timeouts of 1 ms to 17 minutes, as many in each power of two. */
	for (unsigned long i = 0; i < N; i++) {
		unsigned long e = rnd() % 20, ms = (1UL << e) + rnd() % (1UL << e);

		timeout[i] = ms * HZ / 1000 ? ms * HZ / 1000 : 1;
		order[i] = i;
	}
	for (unsigned long i = N - 1; i > 0; i--) {	/* random cancel order */
		unsigned long j = rnd() % (i + 1), tmp = order[i];

		order[i] = order[j];
		order[j] = tmp;
	}

	wheel_init(&w, 0);
	hq_init(&q, slots, N);
	for (unsigned long i = 0; i < N; i++) {
		wtimer_init(&wt[i], wfire);
		htimer_init(&ht[i], hfire);
		wheel_mod(&w, &wt[i], timeout[i], 0);
	}
	m0 = q.moves;
	for (unsigned long i = 0; i < N; i++)
		hq_add(&q, &ht[i], timeout[i], 0);
	adds = q.moves - m0;
	m0 = q.moves;
	for (unsigned long i = 0; i < ncancel; i++) {
		wheel_cancel(&w, &wt[order[i]]);
		hq_del(&q, &ht[order[i]]);
	}
	cancels = q.moves - m0;
	m0 = q.moves;
	wheel_run(&w, 1100000UL * HZ / 1000);
	hq_run(&q, 1100000ULL * HZ / 1000);

	printf("\n%d timeouts of 1 ms to 17 min, %lu cancelled, %lu expired\n",
	       N, ncancel, fired0 + fired);
	printf("wheel, level 0: %lu fired, %lu ticks late in all\n", fired0, late0);
	printf("wheel, levels 1+: %lu fired, late by %.1f%% of the timeout on "
	       "average, %.1f%% at most\n", fired, 100 * late_sum / fired,
	       100 * late_max);
	printf("heap moves per add %.1f, per cancel %.1f, per expiry %.1f\n",
	       (double)adds / N, (double)cancels / ncancel,
	       (double)(q.moves - m0) / (N - ncancel));
}

int main(void)
{
	geometry();
	placement();
	lazy_next();
	workload();
	return 0;
}
