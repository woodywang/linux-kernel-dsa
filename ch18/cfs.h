// SPDX-License-Identifier: MIT
/*
 * cfs.h - a teaching model of the pre-6.6 fair scheduler (CFS) run queue:
 * entities in an rbtree sorted by vruntime, run the leftmost one.  It
 * reuses struct sched_entity and the fixed-point helpers from eevdf.h;
 * the deadline and lag fields are simply unused.
 *
 * What it leaves out: everything eevdf.h leaves out, plus the buddies and
 * the many feature flags of the old code.  The preemption rules that
 * sim.c applies on top of it are described there.
 */
#ifndef CFS_H
#define CFS_H

#include "eevdf.h"

struct cfs_rq {
	struct rb_root_cached	tasks;	/* queued entities, by vruntime */
	struct sched_entity	*curr;	/* running; not in the tree */
	u64		min_vruntime;	/* only ever moves forward */
	unsigned long	load;		/* sum of weights, curr included */
	unsigned int	nr;
};

void cfs_init(struct cfs_rq *rq);
void cfs_enqueue(struct cfs_rq *rq, struct sched_entity *se, u64 sleeper_credit);
void cfs_dequeue(struct cfs_rq *rq, struct sched_entity *se);
struct sched_entity *cfs_pick(struct cfs_rq *rq);
void cfs_set_next(struct cfs_rq *rq, struct sched_entity *se);
void cfs_put_prev(struct cfs_rq *rq);
void cfs_update_curr(struct cfs_rq *rq, u64 delta_exec);

#endif
