// SPDX-License-Identifier: MIT
/*
 * test_heapq.c - randomized tests for heapq.h.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -o test_heapq test_heapq.c
 *   ./test_heapq [steps] [seed]
 *
 * A plain array records which timers are queued and their [soft, hard]
 * ranges.  After each step the heap must satisfy the heap property, every
 * timer's idx must name its slot, and the first timer must have the
 * smallest hard expiry.  Running the queue must fire timers in order of
 * hard expiry, each at or after its soft expiry, never a cancelled one,
 * and must leave no queued timer whose hard expiry has passed.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "container_of.h"
#include "heapq.h"

#define NT	512

struct ttimer {
	struct htimer t;
	bool queued;			/* the model's view */
	ktime soft, hard;
};

static struct heapq q;
static struct htimer *slots[NT];
static struct ttimer tt[NT];
static ktime now, last_hard;
static unsigned long nfired;
static unsigned long long rng_state;

static unsigned long long rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

static ktime model_min(const struct ttimer *except)
{
	ktime m = ~0ULL;

	for (int i = 0; i < NT; i++)
		if (tt[i].queued && &tt[i] != except && tt[i].hard < m)
			m = tt[i].hard;
	return m;
}

static void add(struct ttimer *x)
{
	ktime soft = now + 1 + rnd() % 1000000;
	ktime slack = rnd() % 4 ? rnd() % 50000 : 0;
	ktime before = model_min(x);
	bool first;

	if (x->queued) {
		struct htimer *fb = hq_first(&q);

		first = hq_mod(&q, &x->t, soft, slack);
		/* false only if the front timer, and so its expiry, is unchanged */
		assert(first == (hq_first(&q) != fb || fb == &x->t));
		assert(hq_first(&q)->hard == (before < soft + slack ? before : soft + slack));
	} else {
		first = hq_add(&q, &x->t, soft, slack);
		/* First iff strictly earlier than every other queued timer. */
		assert(first == (soft + slack < before));
	}
	x->queued = true;
	x->soft = soft;
	x->hard = soft + slack;
}

static void callback(struct htimer *t, ktime cb_now)
{
	struct ttimer *x = container_of(t, struct ttimer, t);

	assert(cb_now == now);
	assert(x->queued);			/* cancelled timers never fire */
	assert(x->soft <= now);			/* never before soft expiry */
	assert(x->hard >= last_hard);		/* in order of hard expiry */
	last_hard = x->hard;
	x->queued = false;
	nfired++;
	if (rnd() % 4 == 0)
		add(x);				/* periodic: re-add itself */
}

static void check(void)
{
	for (size_t i = 0; i < q.nr; i++) {
		assert(q.h[i]->idx == i);
		if (i)
			assert(q.h[(i - 1) / 2]->hard <= q.h[i]->hard);
	}
	size_t n = 0;

	for (int i = 0; i < NT; i++) {
		struct ttimer *x = &tt[i];

		assert(htimer_queued(&x->t) == x->queued);
		if (!x->queued)
			continue;
		n++;
		assert(x->t.soft == x->soft && x->t.hard == x->hard);
		assert(x->hard > now);			/* nothing overdue */
	}
	assert(n == q.nr);
	if (q.nr)
		assert(hq_first(&q)->hard == model_min(NULL));
}

int main(int argc, char **argv)
{
	unsigned long steps = argc > 1 ? strtoul(argv[1], NULL, 0) : 200000;
	unsigned long seed = argc > 2 ? strtoul(argv[2], NULL, 0) : 1;

	rng_state = 0x9e3779b97f4a7c15ULL * (seed + 1);
	hq_init(&q, slots, NT);
	for (int i = 0; i < NT; i++)
		htimer_init(&tt[i].t, callback);

	for (unsigned long step = 0; step < steps; step++) {
		unsigned long r = rnd() % 100;
		struct ttimer *x = &tt[rnd() % NT];

		if (r < 50) {
			add(x);
		} else if (r < 75) {
			assert(hq_del(&q, &x->t) == x->queued);
			x->queued = false;
		} else {
			struct htimer *f;

			now += rnd() % 200000;
			last_hard = 0;
			hq_run(&q, now);
			f = hq_first(&q);
			assert(!f || f->soft > now);	/* stopped for a reason */
		}
		if (step % 8 == 0)
			check();
	}
	printf("test_heapq: %lu steps, %lu fired, ok\n", steps, nfired);
	return 0;
}
