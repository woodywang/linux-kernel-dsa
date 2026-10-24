// SPDX-License-Identifier: MIT
/*
 * eevdf.c - the teaching EEVDF run queue (see eevdf.h).
 */
#include "eevdf.h"

/*
 * The kernel's nice-to-weight table: each nice level is about 1.25 times
 * the next, and nice 0 is 1024.
 */
const int sched_prio_to_weight[40] = {
	/* -20 */ 88761, 71755, 56483, 46273, 36291,
	/* -15 */ 29154, 23254, 18705, 14949, 11916,
	/* -10 */  9548,  7620,  6100,  4904,  3906,
	/*  -5 */  3121,  2501,  1991,  1586,  1277,
	/*   0 */  1024,   820,   655,   526,   423,
	/*   5 */   335,   272,   215,   172,   137,
	/*  10 */   110,    87,    70,    56,    45,
	/*  15 */    36,    29,    23,    18,    15,
};

void rq_init(struct eevdf_rq *rq)
{
	*rq = (struct eevdf_rq){ .tasks = RB_ROOT_CACHED };
}

void se_init(struct sched_entity *se, int id, int nice, u64 slice)
{
	*se = (struct sched_entity){ .id = id, .slice = slice };
	RB_CLEAR_NODE(&se->run_node);
	se->weight = sched_prio_to_weight[nice + 20];
	se->inv_weight = WMULT_CONST / se->weight;	/* computed once */
}

/* --- fixed-point virtual time --------------------------------------------- */

static inline u64 mul_u64_u32_shr(u64 a, u32 b, int shift)
{
	return (u64)(((unsigned __int128)a * b) >> shift);
}

/*
 * delta * weight / lw, computed as (delta * (weight * inv)) >> 32, where
 * inv is about 2^32 / lw.  If weight * inv needs more than 32 bits, drop
 * its low bits and shift by that much less, so the product fits in 96.
 */
u64 calc_delta(u64 delta, unsigned long weight, u32 inv_weight)
{
	u64 fact = (u64)weight * inv_weight;
	int shift = WMULT_SHIFT;

	if (fact >> 32) {
		int fs = 64 - __builtin_clzll(fact >> 32);

		shift -= fs;
		fact >>= fs;
	}
	return mul_u64_u32_shr(delta, (u32)fact, shift);
}

/* Virtual time: real time scaled by NICE_0_LOAD / weight. */
u64 calc_delta_fair(u64 delta, const struct sched_entity *se)
{
	if (se->weight != NICE_0_LOAD)
		delta = calc_delta(delta, NICE_0_LOAD, se->inv_weight);
	return delta;
}

/* --- the weighted average V ----------------------------------------------- */

static inline s64 entity_key(const struct eevdf_rq *rq,
			     const struct sched_entity *se)
{
	return (s64)(se->vruntime - rq->zero_vruntime);
}

static void sum_add(struct eevdf_rq *rq, struct sched_entity *se)
{
	rq->sum_w_vruntime += entity_key(rq, se) * (s64)se->weight;
	rq->sum_weight += se->weight;
}

static void sum_sub(struct eevdf_rq *rq, struct sched_entity *se)
{
	rq->sum_w_vruntime -= entity_key(rq, se) * (s64)se->weight;
	rq->sum_weight -= se->weight;
}

/* Moving v0 forward by d lowers every key by d: one multiply fixes the sum. */
static void move_zero_vruntime(struct eevdf_rq *rq, s64 delta)
{
	rq->sum_w_vruntime -= rq->sum_weight * delta;
	rq->zero_vruntime += delta;
}

/* V, rounded down; the running entity counts although it is not in the tree. */
u64 avg_vruntime(const struct eevdf_rq *rq)
{
	struct sched_entity *curr = rq->curr;
	s64 avg = rq->sum_w_vruntime;
	long load = rq->sum_weight;

	if (curr) {
		avg += entity_key(rq, curr) * (s64)curr->weight;
		load += curr->weight;
	}
	if (load) {
		if (avg < 0)		/* round toward minus infinity */
			avg -= load - 1;
		avg /= load;
	}
	return rq->zero_vruntime + avg;
}

/*
 * v <= V  <=>  (v - v0) * W <= sum of (v_i - v0) * w_i.
 * No division, so no rounding: the test is exact.
 */
bool vruntime_eligible(const struct eevdf_rq *rq, u64 vruntime)
{
	struct sched_entity *curr = rq->curr;
	s64 avg = rq->sum_w_vruntime;
	long load = rq->sum_weight;

	if (curr) {
		avg += entity_key(rq, curr) * (s64)curr->weight;
		load += curr->weight;
	}
	return avg >= (s64)(vruntime - rq->zero_vruntime) * load;
}

bool entity_eligible(const struct eevdf_rq *rq, const struct sched_entity *se)
{
	return vruntime_eligible(rq, se->vruntime);
}

/*
 * v6.18 keeps v0 at the queue's smallest vruntime and only moves it
 * forward (the kernel calls it min_vruntime).  The tree's root knows the
 * smallest vruntime in the tree.
 */
static void update_zero_vruntime(struct eevdf_rq *rq)
{
	struct rb_node *root = rq->tasks.rb_root.rb_node;
	struct sched_entity *curr = rq->curr;
	u64 v = rq->zero_vruntime;

	if (curr)
		v = curr->vruntime;
	if (root) {
		u64 m = node_2_se(root)->aug.min_vruntime;

		if (!curr || vruntime_before(m, v))
			v = m;
	}
	if ((s64)(v - rq->zero_vruntime) > 0)
		move_zero_vruntime(rq, (s64)(v - rq->zero_vruntime));
}

/* --- the augmented tree ---------------------------------------------------- */

static inline void aug_child(struct sched_entity *se, struct rb_node *child,
			     typeof(se->aug) *a)
{
	if (child) {
		struct sched_entity *c = node_2_se(child);

		if (vruntime_before(c->aug.min_vruntime, a->min_vruntime))
			a->min_vruntime = c->aug.min_vruntime;
		if (c->aug.min_slice < a->min_slice)
			a->min_slice = c->aug.min_slice;
	}
}

/* Recompute se->aug from se and its children; true if it did not change. */
static inline bool aug_compute(struct sched_entity *se, bool exit)
{
	typeof(se->aug) a = { se->vruntime, se->slice };

	aug_child(se, se->run_node.rb_left, &a);
	aug_child(se, se->run_node.rb_right, &a);
	if (exit && a.min_vruntime == se->aug.min_vruntime &&
	    a.min_slice == se->aug.min_slice)
		return true;
	se->aug = a;
	return false;
}

RB_DECLARE_CALLBACKS(static, aug_cb, struct sched_entity, run_node,
		     aug, aug_compute)

static void __enqueue_entity(struct eevdf_rq *rq, struct sched_entity *se)
{
	struct rb_node **link = &rq->tasks.rb_root.rb_node, *parent = NULL;
	bool leftmost = true;

	sum_add(rq, se);
	se->aug.min_vruntime = se->vruntime;
	se->aug.min_slice = se->slice;
	while (*link) {
		parent = *link;
		if (entity_before(se, node_2_se(parent))) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = false;
		}
	}
	rb_link_node(&se->run_node, parent, link);
	aug_cb.propagate(parent, NULL);		/* ancestors gained a node */
	rb_insert_augmented_cached(&se->run_node, &rq->tasks, leftmost, &aug_cb);
}

static void __dequeue_entity(struct eevdf_rq *rq, struct sched_entity *se)
{
	rb_erase_augmented_cached(&se->run_node, &rq->tasks, &aug_cb);
	RB_CLEAR_NODE(&se->run_node);
	sum_sub(rq, se);
}

/*
 * Earliest eligible virtual deadline first.  The tree is sorted by
 * deadline, and every node knows the smallest vruntime below it, so a
 * subtree holds an eligible entity exactly when its minimum is eligible.
 */
struct sched_entity *pick_eevdf(struct eevdf_rq *rq)
{
	struct rb_node *node = rq->tasks.rb_root.rb_node;
	struct rb_node *first = rb_first_cached(&rq->tasks);
	struct sched_entity *se = first ? node_2_se(first) : NULL;
	struct sched_entity *curr = rq->curr;
	struct sched_entity *best = NULL;

	if (rq->nr == 1)
		return curr ? curr : se;
	if (curr && !entity_eligible(rq, curr))
		curr = NULL;

	/* Fast path: the leftmost entity has the earliest deadline. */
	if (se && entity_eligible(rq, se)) {
		best = se;
		goto found;
	}
	while (node) {
		struct rb_node *left = node->rb_left;

		rq->pick_steps++;
		/* Anything eligible on the left has an earlier deadline. */
		if (left && vruntime_eligible(rq, node_2_se(left)->aug.min_vruntime)) {
			node = left;
			continue;
		}
		se = node_2_se(node);
		if (entity_eligible(rq, se)) {
			best = se;
			break;
		}
		node = node->rb_right;
	}
found:
	if (!best || (curr && entity_before(curr, best)))
		best = curr;
	return best;
}

u64 rq_min_slice(const struct eevdf_rq *rq)
{
	struct rb_node *root = rq->tasks.rb_root.rb_node;
	u64 min_slice = ~0ULL;

	if (rq->curr)
		min_slice = rq->curr->slice;
	if (root && node_2_se(root)->aug.min_slice < min_slice)
		min_slice = node_2_se(root)->aug.min_slice;
	return min_slice;
}

/* --- joining and leaving: lag -------------------------------------------- */

/* vlag = V - v, clamped to two slices (at least a tick) of virtual time. */
static void update_entity_lag(struct eevdf_rq *rq, struct sched_entity *se)
{
	u64 r = 2 * se->slice > TICK_NSEC ? 2 * se->slice : TICK_NSEC;
	s64 limit = (s64)calc_delta_fair(r, se);
	s64 vlag = (s64)(avg_vruntime(rq) - se->vruntime);

	se->vlag = vlag < -limit ? -limit : vlag > limit ? limit : vlag;
}

/*
 * Place se so that after it joins, its lag is the one it left with.
 * Joining moves V by -w*vl/(W+w), so start from vl*(W+w)/W instead.
 */
static void place_entity(struct eevdf_rq *rq, struct sched_entity *se,
			 bool initial)
{
	u64 vslice = calc_delta_fair(se->slice, se);
	u64 vruntime = avg_vruntime(rq);
	s64 lag = 0;

	if (rq->nr && se->vlag) {
		long load = rq->sum_weight;

		if (rq->curr)
			load += rq->curr->weight;
		lag = se->vlag * (s64)(load + se->weight) / load;
	}
	se->vruntime = vruntime - lag;
	if (initial)			/* new tasks start with half a slice */
		vslice /= 2;
	se->deadline = se->vruntime + vslice;
}

void enqueue_entity(struct eevdf_rq *rq, struct sched_entity *se,
		    bool initial)
{
	place_entity(rq, se, initial);
	__enqueue_entity(rq, se);
	se->on_rq = true;
	rq->nr++;
}

void dequeue_entity(struct eevdf_rq *rq, struct sched_entity *se)
{
	update_entity_lag(rq, se);
	if (se == rq->curr)
		rq->curr = NULL;
	else
		__dequeue_entity(rq, se);
	se->on_rq = false;
	rq->nr--;
	update_zero_vruntime(rq);
}

/*
 * Going to sleep.  An entity that is not owed anything (not eligible)
 * stays in the tree, competing, until the pick reaches it; by then its
 * lag is no longer negative.  Returns true if it really left.
 */
bool dequeue_sleep(struct eevdf_rq *rq, struct sched_entity *se)
{
	if (!entity_eligible(rq, se)) {
		se->sched_delayed = true;
		return false;
	}
	dequeue_entity(rq, se);
	return true;
}

/* The pick found a sleeping entity: now it leaves, keeping no credit. */
void finish_delayed_dequeue(struct eevdf_rq *rq, struct sched_entity *se)
{
	dequeue_entity(rq, se);
	se->sched_delayed = false;
	if (se->vlag > 0)
		se->vlag = 0;
}

/* Woken while still queued: keep its debt, but not any credit. */
void requeue_delayed(struct eevdf_rq *rq, struct sched_entity *se)
{
	se->sched_delayed = false;
	if (entity_eligible(rq, se)) {
		dequeue_entity(rq, se);
		se->vlag = 0;
		enqueue_entity(rq, se, false);
	}
}

/* --- running ---------------------------------------------------------------- */

/* Protect the new entity's slice, but only for the shortest queued slice. */
static void set_protect_slice(struct eevdf_rq *rq, struct sched_entity *se)
{
	u64 slice = rq_min_slice(rq);

	se->vprot = se->deadline;
	if (slice < se->slice) {
		u64 v = se->vruntime + calc_delta_fair(slice, se);

		if (vruntime_before(v, se->vprot))
			se->vprot = v;
	}
}

void update_protect_slice(struct eevdf_rq *rq, struct sched_entity *se)
{
	u64 v = se->vruntime + calc_delta_fair(rq_min_slice(rq), se);

	if (vruntime_before(v, se->vprot))
		se->vprot = v;
}

bool protect_slice(const struct sched_entity *se)
{
	return vruntime_before(se->vruntime, se->vprot);
}

void cancel_protect_slice(struct sched_entity *se)
{
	if (protect_slice(se))
		se->vprot = se->vruntime;
}

/* The running entity leaves the tree, but still counts toward V. */
void set_next_entity(struct eevdf_rq *rq, struct sched_entity *se)
{
	if (se == rq->curr)
		return;
	__dequeue_entity(rq, se);
	set_protect_slice(rq, se);
	rq->curr = se;
}

void put_prev_entity(struct eevdf_rq *rq)
{
	if (rq->curr && rq->curr->on_rq)
		__enqueue_entity(rq, rq->curr);
	rq->curr = NULL;
}

/* A new request once the old one is used up: vd = v + r/w. */
static bool update_deadline(struct sched_entity *se)
{
	if (vruntime_before(se->vruntime, se->deadline))
		return false;
	se->deadline = se->vruntime + calc_delta_fair(se->slice, se);
	return true;
}

/* Charge delta_exec ns to the running entity; true if its slice ran out. */
bool update_curr(struct eevdf_rq *rq, u64 delta_exec)
{
	struct sched_entity *curr = rq->curr;
	bool resched;

	curr->vruntime += calc_delta_fair(delta_exec, curr);
	resched = update_deadline(curr);
	update_zero_vruntime(rq);
	return resched;
}
