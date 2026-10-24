// SPDX-License-Identifier: MIT
/*
 * sim.c - a deterministic one-CPU simulation that runs the same synthetic
 * workload under a CFS-style policy and under EEVDF, and reports how long
 * periodic task waits after each wakeup, and how the CPU was shared.
 *
 * It is a model, not a measurement of a kernel.  Time advances from event
 * to event (a tick every millisecond, a wakeup, the end of a burst); the
 * scheduler acts only at those points, and a reschedule takes effect at
 * once (no context-switch cost, no lazy preemption).
 *
 * CFS-style rules (after Linux 6.5, tunables scaled for 8 CPUs):
 *   - pick the smallest vruntime;
 *   - a waking task starts no earlier than min_vruntime - latency/2;
 *   - it preempts if it is more than wakeup_gran (in its own virtual
 *     time) behind the running task;
 *   - at a tick, the running task is preempted after its share of the
 *     latency period, or when it is far enough ahead of the leftmost task.
 * EEVDF rules (after Linux 6.18): those of eevdf.c, including delayed
 * dequeue, with wakeup preemption when the woken task becomes the pick,
 * slice protection, and the shorter-slice override (PREEMPT_SHORT).
 */
#include <stdio.h>
#include <string.h>

#include "cfs.h"
#include "eevdf.h"

#define MS		1000000ULL
#define US		1000ULL
#define TICK		(1 * MS)
#define END		(10000 * MS)	/* 10 seconds */
#define WARMUP		(100 * MS)	/* wakeups before this are not counted */

/* Tunables as the kernel scales them on 8 CPUs: factor 1 + log2(8) = 4. */
#define BASE_SLICE	(2800 * US)	/* EEVDF: 0.7 ms * 4 */
#define LATENCY		(24 * MS)	/* CFS: 6 ms * 4 */
#define MIN_GRAN	(3 * MS)	/* CFS: 0.75 ms * 4 */
#define WAKEUP_GRAN	(4 * MS)	/* CFS: 1 ms * 4 */
#define NR_LATENCY	8		/* CFS: LATENCY / MIN_GRAN */

struct task {
	struct sched_entity se;
	const char *name;
	u64 period, burst;		/* burst 0: always runnable */
	u64 left, next_wake, woke_at;
	bool sleeping, waiting;
	u64 ran, lat_sum, lat_max, picked_at;
	unsigned long wakeups, late;	/* late: waited 1 ms or more */
};

#define NR_TASKS 4
static struct task tasks[NR_TASKS];
static struct task *curr;
static bool need_resched;
static int mode;			/* 0: CFS, 1: EEVDF */
static struct cfs_rq crq;
static struct eevdf_rq erq;

static struct task *task_of(struct sched_entity *se)
{
	return se ? container_of(se, struct task, se) : NULL;
}

/* --- CFS-style policy ------------------------------------------------------- */

static u64 cfs_sched_slice(struct sched_entity *se)
{
	u64 period = crq.nr > NR_LATENCY ? crq.nr * MIN_GRAN : LATENCY;

	return period * se->weight / crq.load;
}

static void cfs_wake(struct task *t)
{
	cfs_enqueue(&crq, &t->se, LATENCY / 2);
	if (curr && (s64)(curr->se.vruntime - t->se.vruntime) >
		    (s64)calc_delta_fair(WAKEUP_GRAN, &t->se))
		need_resched = true;
}

static void cfs_tick(u64 now)
{
	u64 ideal = cfs_sched_slice(&curr->se), ran = now - curr->picked_at;
	struct rb_node *left = rb_first_cached(&crq.tasks);

	if (ideal > LATENCY)
		ideal = LATENCY;
	if (ran > ideal) {
		need_resched = true;
	} else if (left && ran >= MIN_GRAN &&
		   (s64)(curr->se.vruntime - node_2_se(left)->vruntime) > (s64)ideal) {
		need_resched = true;
	}
}

/* --- EEVDF policy ------------------------------------------------------------ */

/* pick_eevdf(), but keep a protected, eligible current task. */
static struct sched_entity *eevdf_pick_protect(bool protect)
{
	struct sched_entity *c = erq.curr;

	if (erq.nr > 1 && protect && c && entity_eligible(&erq, c) &&
	    protect_slice(c))
		return c;
	return pick_eevdf(&erq);
}

static void eevdf_wake(struct task *t)
{
	struct sched_entity *c = erq.curr;
	bool preempt_short;

	if (t->se.sched_delayed)
		requeue_delayed(&erq, &t->se);	/* still queued: wake in place */
	else
		enqueue_entity(&erq, &t->se, false);
	if (!c)
		return;
	preempt_short = t->se.slice < c->slice;
	if (eevdf_pick_protect(!preempt_short) == &t->se) {
		if (preempt_short)
			cancel_protect_slice(c);
		need_resched = true;
	} else if (preempt_short) {
		update_protect_slice(&erq, c);
	}
}

/* --- the simulation ------------------------------------------------------------ */

static void charge(u64 delta)
{
	if (!curr || !delta)
		return;
	curr->ran += delta;
	if (curr->burst)
		curr->left -= delta;
	if (mode == 0) {
		cfs_update_curr(&crq, delta);
	} else if (update_curr(&erq, delta) || !protect_slice(&curr->se)) {
		if (erq.nr > 1)
			need_resched = true;
	}
}

static void schedule(u64 now)
{
	struct sched_entity *next;

	if (mode == 0) {
		next = cfs_pick(&crq);
		cfs_put_prev(&crq);
		if (next)
			cfs_set_next(&crq, next);
	} else {
		for (;;) {
			next = erq.nr ? eevdf_pick_protect(true) : NULL;
			if (!next || !next->sched_delayed)
				break;
			finish_delayed_dequeue(&erq, next);
		}
		put_prev_entity(&erq);
		if (next)
			set_next_entity(&erq, next);
	}
	if (task_of(next) != curr && next)
		task_of(next)->picked_at = now;
	curr = task_of(next);
	need_resched = false;
	if (curr && curr->waiting) {
		u64 lat = now - curr->woke_at;

		curr->waiting = false;
		if (curr->woke_at < WARMUP)
			return;
		curr->wakeups++;
		curr->lat_sum += lat;
		if (lat > curr->lat_max)
			curr->lat_max = lat;
		if (lat >= 1 * MS)
			curr->late++;
	}
}

static u64 burst, period;		/* the periodic task's workload */

static void run(int m, u64 short_slice)
{
	u64 now = 0, next_tick = TICK;

	mode = m;
	cfs_init(&crq);
	rq_init(&erq);
	curr = NULL;
	need_resched = false;
	for (int i = 0; i < NR_TASKS; i++) {
		struct task *t = &tasks[i];
		u64 slice = i == NR_TASKS - 1 && short_slice ? short_slice : BASE_SLICE;

		memset(t, 0, sizeof(*t));
		se_init(&t->se, i, 0, slice);
		t->name = "hog";
		if (i == NR_TASKS - 1) {	/* starts with its first burst */
			t->name = "periodic";
			t->period = period;
			t->burst = burst;
			t->left = t->burst;
			t->waiting = true;
		}
		if (m == 0)
			cfs_enqueue(&crq, &t->se, 0);
		else
			enqueue_entity(&erq, &t->se, true);
	}

	while (now < END) {
		u64 te = next_tick;

		if (need_resched || !curr)
			schedule(now);
		for (int i = 0; i < NR_TASKS; i++)
			if (tasks[i].sleeping && tasks[i].next_wake < te)
				te = tasks[i].next_wake;
		if (curr && curr->burst && now + curr->left < te)
			te = now + curr->left;

		charge(te - now);
		now = te;

		if (curr && curr->burst && !curr->left) {	/* burst done */
			if (m == 0)
				cfs_dequeue(&crq, &curr->se);
			else
				dequeue_sleep(&erq, &curr->se);
			curr->sleeping = true;
			/* Wake at the next period boundary; skip missed ones. */
			curr->next_wake = curr->woke_at + curr->period;
			while (curr->next_wake <= now)
				curr->next_wake += curr->period;
			curr = NULL;
			schedule(now);
		}
		for (int i = 0; i < NR_TASKS; i++) {
			struct task *t = &tasks[i];

			if (!t->sleeping || t->next_wake != now)
				continue;
			t->sleeping = false;
			t->waiting = true;
			t->woke_at = now;
			t->left = t->burst;
			if (m == 0)
				cfs_wake(t);
			else
				eevdf_wake(t);
		}
		if (now == next_tick) {
			if (curr && m == 0)
				cfs_tick(now);
			next_tick += TICK;
		}
	}
}

static void report(const char *label)
{
	struct task *p = &tasks[NR_TASKS - 1];
	u64 hogs = 0;

	for (int i = 0; i < NR_TASKS - 1; i++)
		hogs += tasks[i].ran;
	printf("%-22s %4llu.%02llu ms %4llu.%02llu ms %6lu of %-5lu %6llu%% / %llu%%\n",
	       label, (p->lat_sum / p->wakeups) / MS,
	       (p->lat_sum / p->wakeups) % MS / (10 * US),
	       p->lat_max / MS, p->lat_max % MS / (10 * US),
	       p->late, p->wakeups, p->ran * 100 / END, hogs * 100 / END);
}

static void workload(u64 b, u64 p)
{
	burst = b;
	period = p;
	printf("3 CPU-bound tasks and 1 task that needs %llu ms every %llu ms:\n",
	       b / MS, p / MS);
	run(0, 0);
	report("  CFS-style");
	run(1, 0);
	report("  EEVDF, 2.8 ms slice");
	run(1, 1 * MS);
	report("  EEVDF, 1 ms slice");
	run(1, 300 * US);
	report("  EEVDF, 0.3 ms slice");
}

int main(void)
{
	printf("One CPU, 10 s; wakeups in the first 0.1 s are not counted\n");
	printf("%-22s %10s %10s %14s  %s\n", "policy", "avg wait",
	       "max wait", "waits >= 1 ms", "CPU: task/hogs");
	workload(1 * MS, 5 * MS);	/* Exercise 18.4: try others */
	return 0;
}
