// SPDX-License-Identifier: MIT
/*
 * bench.c - time the wheel against the heap (Exercise 19.3).
 *
 *   make bench && ./bench [timers] [percent cancelled]
 *
 * Arms the given number of timeouts (1 ms to 17 minutes, as many in each
 * power of two), cancels the given share of them in random order, then
 * runs the rest to expiry, once with the wheel and once with the heap.
 * Timings depend on your machine; they are not quoted in the chapter.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "heapq.h"
#include "wheel.h"

static unsigned long long rng_state = 7;

static unsigned long rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (unsigned long)rng_state;
}

static double secs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void wnop(struct wtimer *t, unsigned long tick)
{
	(void)t;
	(void)tick;
}

static void hnop(struct htimer *t, ktime now)
{
	(void)t;
	(void)now;
}

int main(int argc, char **argv)
{
	unsigned long n = argc > 1 ? strtoul(argv[1], NULL, 0) : 1000000;
	unsigned long pct = argc > 2 ? strtoul(argv[2], NULL, 0) : 95;
	unsigned long *tmo = malloc(n * sizeof(*tmo)), *order = malloc(n * sizeof(*order));
	struct wtimer *wt = malloc(n * sizeof(*wt));
	struct htimer *ht = malloc(n * sizeof(*ht));
	struct htimer **slots = malloc(n * sizeof(*slots));
	static struct wheel w;
	struct heapq q;
	unsigned long nc = n / 100 * pct;
	double t0, t1, t2;

	if (!tmo || !order || !wt || !ht || !slots)
		return 1;
	for (unsigned long i = 0; i < n; i++) {
		unsigned long e = rnd() % 20;

		tmo[i] = (1UL << e) + rnd() % (1UL << e);
		order[i] = i;
	}
	for (unsigned long i = n - 1; i > 0; i--) {
		unsigned long j = rnd() % (i + 1), tmp = order[i];

		order[i] = order[j];
		order[j] = tmp;
	}

	wheel_init(&w, 0);
	t0 = secs();
	for (unsigned long i = 0; i < n; i++) {
		wtimer_init(&wt[i], wnop);
		wheel_mod(&w, &wt[i], tmo[i], 0);
	}
	t1 = secs();
	for (unsigned long i = 0; i < nc; i++)
		wheel_cancel(&w, &wt[order[i]]);
	t2 = secs();
	wheel_run(&w, 1UL << 21);
	printf("wheel: add %.1f ns, cancel %.1f ns, expire all %.3f s\n",
	       (t1 - t0) * 1e9 / n, (t2 - t1) * 1e9 / (nc ? nc : 1), secs() - t2);

	hq_init(&q, slots, n);
	t0 = secs();
	for (unsigned long i = 0; i < n; i++) {
		htimer_init(&ht[i], hnop);
		hq_add(&q, &ht[i], tmo[i], 0);
	}
	t1 = secs();
	for (unsigned long i = 0; i < nc; i++)
		hq_del(&q, &ht[order[i]]);
	t2 = secs();
	hq_run(&q, 1ULL << 21);
	printf("heap:  add %.1f ns, cancel %.1f ns, expire all %.3f s\n",
	       (t1 - t0) * 1e9 / n, (t2 - t1) * 1e9 / (nc ? nc : 1), secs() - t2);
	return 0;
}
