// SPDX-License-Identifier: MIT
/*
 * cfs.c - the CFS-style run queue (see cfs.h).
 */
#include "cfs.h"

void cfs_init(struct cfs_rq *rq)
{
	*rq = (struct cfs_rq){ .tasks = RB_ROOT_CACHED };
}

static void update_min_vruntime(struct cfs_rq *rq)
{
	struct rb_node *left = rb_first_cached(&rq->tasks);
	u64 v = rq->min_vruntime;

	if (rq->curr)
		v = rq->curr->vruntime;
	if (left) {
		u64 lv = node_2_se(left)->vruntime;

		if (!rq->curr || vruntime_before(lv, v))
			v = lv;
	}
	if (vruntime_before(rq->min_vruntime, v))
		rq->min_vruntime = v;
}

static void __cfs_enqueue(struct cfs_rq *rq, struct sched_entity *se)
{
	struct rb_node **link = &rq->tasks.rb_root.rb_node, *parent = NULL;
	bool leftmost = true;

	while (*link) {
		parent = *link;
		if (vruntime_before(se->vruntime, node_2_se(parent)->vruntime)) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = false;
		}
	}
	rb_link_node(&se->run_node, parent, link);
	rb_insert_color_cached(&se->run_node, &rq->tasks, leftmost);
}

/*
 * A waking task restarts no earlier than min_vruntime minus a credit, so
 * a long sleep does not buy unlimited CPU time.
 */
void cfs_enqueue(struct cfs_rq *rq, struct sched_entity *se, u64 sleeper_credit)
{
	u64 v = rq->min_vruntime - sleeper_credit;

	if (vruntime_before(se->vruntime, v))
		se->vruntime = v;
	__cfs_enqueue(rq, se);
	se->on_rq = true;
	rq->load += se->weight;
	rq->nr++;
}

void cfs_dequeue(struct cfs_rq *rq, struct sched_entity *se)
{
	if (se == rq->curr)
		rq->curr = NULL;
	else
		rb_erase_cached(&se->run_node, &rq->tasks);
	se->on_rq = false;
	rq->load -= se->weight;
	rq->nr--;
	update_min_vruntime(rq);
}

/* The whole pick: the task that has run least, in virtual time. */
struct sched_entity *cfs_pick(struct cfs_rq *rq)
{
	struct rb_node *left = rb_first_cached(&rq->tasks);
	struct sched_entity *se = left ? node_2_se(left) : NULL;

	if (!se || (rq->curr && !vruntime_before(se->vruntime, rq->curr->vruntime)))
		return rq->curr;
	return se;
}

void cfs_set_next(struct cfs_rq *rq, struct sched_entity *se)
{
	if (se == rq->curr)
		return;
	rb_erase_cached(&se->run_node, &rq->tasks);
	rq->curr = se;
}

void cfs_put_prev(struct cfs_rq *rq)
{
	if (rq->curr && rq->curr->on_rq)
		__cfs_enqueue(rq, rq->curr);
	rq->curr = NULL;
}

void cfs_update_curr(struct cfs_rq *rq, u64 delta_exec)
{
	rq->curr->vruntime += calc_delta_fair(delta_exec, rq->curr);
	update_min_vruntime(rq);
}
