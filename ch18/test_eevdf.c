// SPDX-License-Identifier: MIT
/*
 * test_eevdf.c - randomized test of the teaching EEVDF run queue.
 *
 *   ./test_eevdf [operations] [seed]
 *
 * Entities with random nice values and slices join, leave, run and are
 * switched.  After every operation the test recomputes everything from
 * scratch, with 128-bit arithmetic, and compares:
 *   - the red-black rules, parent pointers, cached leftmost node, and the
 *     deadline order of the tree;
 *   - every augmented value (subtree minimum vruntime and slice);
 *   - the incremental sums against fresh sums, and avg_vruntime() against
 *     the exact weighted average, rounded down;
 *   - the eligibility of every queued entity against the exact test;
 *   - pick_eevdf() against a brute-force scan of the eligible set (the
 *     model), and its number of steps against the tree height;
 *   - that placement restores the lag an entity left with (within 2 ns);
 *   - the fixed-point calc_delta_fair() against exact division.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "eevdf.h"

#define POOL	300

typedef __int128 s128;

static unsigned long long rng;

static unsigned long rnd(unsigned long n)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (unsigned long)(rng % n);
}

static struct sched_entity pool[POOL];
static struct eevdf_rq rq;

/* --- tree checks ---------------------------------------------------------- */

static int check_node(struct rb_node *n, struct rb_node *parent,
		      unsigned long *count, int *height, int depth)
{
	struct sched_entity *se;
	u64 mv, ms;
	int lh, rh;

	if (!n)
		return 1;
	if (depth > *height)
		*height = depth;
	assert(rb_parent(n) == parent);
	if (rb_is_red(n)) {
		assert(!n->rb_left || rb_is_black(n->rb_left));
		assert(!n->rb_right || rb_is_black(n->rb_right));
	}
	lh = check_node(n->rb_left, n, count, height, depth + 1);
	rh = check_node(n->rb_right, n, count, height, depth + 1);
	assert(lh == rh);

	se = node_2_se(n);
	assert(se->on_rq && se != rq.curr);
	mv = se->vruntime;
	ms = se->slice;
	for (struct rb_node *c = n->rb_left; c; c = NULL) {
		struct sched_entity *k = node_2_se(c);

		assert(!entity_before(se, k));		/* left <= node */
		mv = vruntime_before(k->aug.min_vruntime, mv) ? k->aug.min_vruntime : mv;
		ms = k->aug.min_slice < ms ? k->aug.min_slice : ms;
	}
	for (struct rb_node *c = n->rb_right; c; c = NULL) {
		struct sched_entity *k = node_2_se(c);

		assert(!entity_before(k, se));		/* node <= right */
		mv = vruntime_before(k->aug.min_vruntime, mv) ? k->aug.min_vruntime : mv;
		ms = k->aug.min_slice < ms ? k->aug.min_slice : ms;
	}
	assert(se->aug.min_vruntime == mv);
	assert(se->aug.min_slice == ms);
	(*count)++;
	return lh + rb_is_black(n);
}

/* Exact: sum of (v_i - v0) * w_i and sum of w_i, curr included. */
static void exact_sums(s128 *sum, s128 *load)
{
	*sum = 0;
	*load = 0;
	for (int i = 0; i < POOL; i++) {
		if (!pool[i].on_rq)
			continue;
		*sum += (s128)(s64)(pool[i].vruntime - rq.zero_vruntime) *
			pool[i].weight;
		*load += pool[i].weight;
	}
}

static bool exact_eligible(u64 v, s128 sum, s128 load)
{
	return sum >= (s128)(s64)(v - rq.zero_vruntime) * load;
}

static unsigned long nr_picks, nr_fast, max_steps;

static void check_all(void)
{
	unsigned long count = 0, in_tree = 0, steps;
	int height = 0;
	s128 sum, load, tsum = 0, tload = 0;
	struct rb_node *first = rb_first_cached(&rq.tasks);
	struct sched_entity *best = NULL, *pick;

	check_node(rq.tasks.rb_root.rb_node, NULL, &count, &height, 1);
	assert(first == rb_first(&rq.tasks.rb_root));

	exact_sums(&sum, &load);
	for (int i = 0; i < POOL; i++) {
		struct sched_entity *se = &pool[i];

		if (!se->on_rq || se == rq.curr)
			continue;
		in_tree++;
		tsum += (s128)(s64)(se->vruntime - rq.zero_vruntime) * se->weight;
		tload += se->weight;
	}
	assert(count == in_tree);
	assert(rq.nr == in_tree + (rq.curr != NULL));
	assert(tsum == rq.sum_w_vruntime && tload == rq.sum_weight);
	if (!rq.nr)
		return;

	/* avg_vruntime() is the exact average, rounded down. */
	{
		s128 q = sum / load;

		if (sum % load && sum < 0)
			q--;
		assert(avg_vruntime(&rq) == rq.zero_vruntime + (u64)(s64)q);
	}

	/* The model: scan every queued entity. */
	for (int i = 0; i < POOL; i++) {
		struct sched_entity *se = &pool[i];

		if (!se->on_rq)
			continue;
		assert(entity_eligible(&rq, se) ==
		       exact_eligible(se->vruntime, sum, load));
		if (exact_eligible(se->vruntime, sum, load) &&
		    (!best || entity_before(se, best)))
			best = se;
	}
	assert(best);		/* the smallest vruntime is always eligible */

	steps = rq.pick_steps;
	pick = pick_eevdf(&rq);
	steps = rq.pick_steps - steps;
	nr_picks++;
	if (!steps)
		nr_fast++;
	if (steps > max_steps)
		max_steps = steps;
	assert(steps <= (unsigned long)height);
	assert(pick && pick->on_rq);
	assert(exact_eligible(pick->vruntime, sum, load));
	assert(pick->deadline == best->deadline);
}

/* --- operations ------------------------------------------------------------- */

static s128 max_err;
static unsigned int max_nr;

/* Error of the multiply-and-shift against exact division. */
static void check_calc_delta(void)
{
	struct sched_entity *se = &pool[rnd(POOL)];
	u64 delta = 1000000 + rnd(50000000);
	s128 exact = (s128)delta * NICE_0_LOAD / se->weight;
	s128 err = (s128)calc_delta_fair(delta, se) - exact;

	if (err < 0)
		err = -err;
	assert(err <= 1 + exact / 40000);	/* within 25 ppm */
	if (err > max_err)
		max_err = err;
}

static void op_enqueue(void)
{
	struct sched_entity *se = &pool[rnd(POOL)];
	bool initial = rnd(4) == 0;
	unsigned int nr = rq.nr;
	s64 vlag;

	if (se->on_rq) {
		if (se->sched_delayed)		/* a wakeup */
			requeue_delayed(&rq, se);
		return;
	}
	if (initial)
		se->vlag = 0;
	vlag = se->vlag;
	enqueue_entity(&rq, se, initial);
	/* Placement preserves lag: V - v after joining is the saved vlag. */
	if (nr) {
		s64 now = (s64)(avg_vruntime(&rq) - se->vruntime);

		assert(now - vlag <= 2 && vlag - now <= 2);
	}
}

static void op_dequeue(void)
{
	struct sched_entity *se = &pool[rnd(POOL)];
	u64 r;
	s64 limit;

	if (!se->on_rq)
		return;
	if (se->sched_delayed) {
		finish_delayed_dequeue(&rq, se);
		assert(se->vlag <= 0);
		return;
	}
	dequeue_entity(&rq, se);
	r = 2 * se->slice > TICK_NSEC ? 2 * se->slice : TICK_NSEC;
	limit = (s64)calc_delta_fair(r, se);
	assert(se->vlag <= limit && se->vlag >= -limit);
}

static unsigned long nr_delayed, nr_finished;

static void schedule(void)
{
	struct sched_entity *next;

	for (;;) {
		if (!rq.nr) {
			put_prev_entity(&rq);
			return;
		}
		next = pick_eevdf(&rq);
		if (!next->sched_delayed)
			break;
		finish_delayed_dequeue(&rq, next);	/* asleep: let it go */
		nr_finished++;
		check_all();
	}
	put_prev_entity(&rq);
	set_next_entity(&rq, next);
}

/* The running entity blocks. */
static void op_sleep(void)
{
	if (!rq.curr)
		return;
	if (!dequeue_sleep(&rq, rq.curr))
		nr_delayed++;
	check_all();
	schedule();
}

static void op_run(void)
{
	if (!rq.curr) {
		schedule();
		return;
	}
	if (update_curr(&rq, 1 + rnd(4000000)) || rnd(3) == 0)
		schedule();
}

int main(int argc, char **argv)
{
	long ops = argc > 1 ? atol(argv[1]) : 100000;
	unsigned long seed = argc > 2 ? strtoul(argv[2], NULL, 0) : 1;

	rng = 0x9E3779B97F4A7C15ULL ^ (seed * 0x2545F4914F6CDD1DULL);
	rq_init(&rq);
	for (int i = 0; i < POOL; i++) {
		static const u64 slices[] = { 100000, 700000, 2800000,
					      3000000, 10000000, 100000000 };

		int nice = (int)rnd(40) - 20;

		se_init(&pool[i], i, nice, slices[rnd(6)]);
	}
	/* Start the clock near the wrap point to exercise the signed compares. */
	rq.zero_vruntime = (u64)-20000000LL;

	for (long n = 0; n < ops; n++) {
		/* Phases: grow the queue, shrink it, then grow again. */
		long phase = n * 3 / (ops ? ops : 1);
		unsigned long r = rnd(10);

		if (r < (phase == 1 ? 2u : 4u))
			op_enqueue();
		else if (r < 5)
			op_dequeue();
		else if (r < 6)
			op_sleep();
		else
			op_run();
		check_all();
		check_calc_delta();
		if (rq.nr > max_nr)
			max_nr = rq.nr;
	}
	printf("ok: %ld operations, seed %lu, up to %u queued: %lu picks "
	       "checked, %lu%% on the fast path, at most %lu tree steps; "
	       "calc_delta_fair() within %ld ns of exact; v0 wrapped: %s; "
	       "%lu delayed dequeues, %lu finished by the pick\n",
	       ops, seed, max_nr, nr_picks,
	       nr_picks ? nr_fast * 100 / nr_picks : 0, max_steps,
	       (long)max_err, rq.zero_vruntime < (u64)1 << 63 ? "yes" : "no",
	       nr_delayed, nr_finished);
	return 0;
}
