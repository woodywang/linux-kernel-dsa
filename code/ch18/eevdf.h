// SPDX-License-Identifier: MIT
/*
 * eevdf.h - a teaching version of the run queue of Linux's fair scheduler
 * (EEVDF, Linux 6.6 and later), following the v6.18 design:
 *
 *   - weights come from the kernel's nice-to-weight table, and virtual
 *     time is advanced with a fixed-point inverse weight (multiply and
 *     shift) instead of a division; no floating point anywhere;
 *   - the queue keeps, incrementally, the weighted sum of every queued
 *     entity's virtual runtime relative to a zero point, so the weighted
 *     average V is one division away, and eligibility (v <= V) needs none;
 *   - the queued entities sit in an rbtree sorted by virtual deadline and
 *     augmented with the smallest vruntime (and smallest slice) in each
 *     subtree, so the earliest eligible deadline is found in O(log n);
 *   - the running entity is kept out of the tree, as in the kernel;
 *   - lag is saved when an entity leaves and restored when it returns;
 *   - a task that goes to sleep while it is ineligible stays queued
 *     ("delayed dequeue") until it is picked or woken, so that its
 *     negative lag is paid off rather than carried across the sleep.
 *
 * What it leaves out on purpose: group scheduling (one level only),
 * buddies, reweighting a queued entity, load tracking,
 * multiple CPUs, and locking.  Weights use the 10-bit resolution of the
 * nice table (the kernel's 64-bit builds keep 10 more bits internally and
 * scale them down for these sums).  Times are in nanoseconds.
 */
#ifndef EEVDF_H
#define EEVDF_H

#include <stdbool.h>
#include <stdint.h>
#include "rbtree.h"

typedef uint64_t u64;
typedef int64_t s64;
typedef uint32_t u32;

#define NICE_0_LOAD	1024UL
#define WMULT_CONST	(~0U)		/* 2^32 - 1 */
#define WMULT_SHIFT	32
#define TICK_NSEC	1000000ULL	/* HZ=1000 */

extern const int sched_prio_to_weight[40];

struct sched_entity {
	struct rb_node	run_node;
	u64		deadline;	/* virtual deadline: the tree's key */
	struct {
		u64	min_vruntime;	/* smallest vruntime in the subtree */
		u64	min_slice;	/* smallest slice in the subtree */
	} aug;				/* one field, so one copy moves both */
	u64		vruntime;
	s64		vlag;		/* V - vruntime, saved on dequeue */
	u64		vprot;		/* run undisturbed until here */
	u64		slice;		/* requested slice (ns of real time) */
	unsigned long	weight;
	u32		inv_weight;	/* about 2^32 / weight */
	bool		on_rq;
	bool		sched_delayed;	/* asleep, but still queued */
	int		id;
};

struct eevdf_rq {
	struct rb_root_cached	tasks;	/* queued entities, by deadline */
	struct sched_entity	*curr;	/* running; not in the tree */
	u64		zero_vruntime;	/* v0: keys are vruntime - v0 */
	s64		sum_w_vruntime;	/* sum of (v_i - v0) * w_i in the tree */
	long		sum_weight;	/* sum of w_i in the tree */
	unsigned int	nr;		/* queued entities, curr included */
	unsigned long	pick_steps;	/* nodes visited by tree searches */
};

#define node_2_se(n)	rb_entry((n), struct sched_entity, run_node)

/* Wrap-safe comparison of virtual times. */
static inline bool vruntime_before(u64 a, u64 b)
{
	return (s64)(a - b) < 0;
}

static inline bool entity_before(const struct sched_entity *a,
				 const struct sched_entity *b)
{
	return vruntime_before(a->deadline, b->deadline);
}

void rq_init(struct eevdf_rq *rq);
void se_init(struct sched_entity *se, int id, int nice, u64 slice);

u64 calc_delta(u64 delta, unsigned long weight, u32 inv_weight);
u64 calc_delta_fair(u64 delta, const struct sched_entity *se);

u64 avg_vruntime(const struct eevdf_rq *rq);
bool vruntime_eligible(const struct eevdf_rq *rq, u64 vruntime);
bool entity_eligible(const struct eevdf_rq *rq, const struct sched_entity *se);
u64 rq_min_slice(const struct eevdf_rq *rq);

void enqueue_entity(struct eevdf_rq *rq, struct sched_entity *se,
		    bool initial);
void dequeue_entity(struct eevdf_rq *rq, struct sched_entity *se);
bool dequeue_sleep(struct eevdf_rq *rq, struct sched_entity *se);
void finish_delayed_dequeue(struct eevdf_rq *rq, struct sched_entity *se);
void requeue_delayed(struct eevdf_rq *rq, struct sched_entity *se);
struct sched_entity *pick_eevdf(struct eevdf_rq *rq);
void set_next_entity(struct eevdf_rq *rq, struct sched_entity *se);
void put_prev_entity(struct eevdf_rq *rq);
bool update_curr(struct eevdf_rq *rq, u64 delta_exec);

bool protect_slice(const struct sched_entity *se);
void update_protect_slice(struct eevdf_rq *rq, struct sched_entity *se);
void cancel_protect_slice(struct sched_entity *se);

#endif
