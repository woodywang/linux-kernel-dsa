// SPDX-License-Identifier: MIT
/*
 * urcu.c - grace periods, call_rcu() and rcu_barrier() for urcu.h.
 *
 * Build with -DURCU_BROKEN to get a grace period that does not wait for
 * readers (Exercise 13.2); the stress test must then fail under ASan.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "urcu.h"

_Thread_local struct rcu_reader rcu_me;
_Atomic unsigned long rcu_gp_seq = 2;	/* even: idle; never 0 */
#ifdef URCU_TSAN
_Atomic unsigned long urcu_tsan_fence;
#endif

static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct rcu_reader *registry;

static pthread_mutex_t gp_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long n_gp, n_gp_waits, n_gp_shared;

/* ---- registration ----------------------------------------------------- */

void rcu_register_thread(void)
{
	pthread_mutex_lock(&registry_lock);
	rcu_me.registered = true;
	rcu_me.next = registry;
	registry = &rcu_me;
	pthread_mutex_unlock(&registry_lock);
}

void rcu_unregister_thread(void)
{
	pthread_mutex_lock(&registry_lock);
	for (struct rcu_reader **pp = &registry; *pp; pp = &(*pp)->next)
		if (*pp == &rcu_me) {
			*pp = rcu_me.next;
			break;
		}
	rcu_me.registered = false;
	pthread_mutex_unlock(&registry_lock);
}

/* ---- grace periods ---------------------------------------------------- */

static void wait_a_little(int spins)
{
	if (spins < 100) {
		sched_yield();
	} else {
		struct timespec ts = { 0, 50000 };	/* 50 us */

		nanosleep(&ts, NULL);
	}
}

/*
 * Run one grace period.  Called with gp_lock held, so no other grace
 * period is in progress and rcu_gp_seq is even.
 */
static void run_one_gp(void)
{
	unsigned long start = atomic_load_explicit(&rcu_gp_seq,
						   memory_order_relaxed) + 1;

	smp_mb();		/* removals before the counter moves */
	atomic_store_explicit(&rcu_gp_seq, start, memory_order_seq_cst);
	smp_mb();		/* counter moves before we look at readers */

	pthread_mutex_lock(&registry_lock);
	for (struct rcu_reader *r = registry; r; r = r->next) {
		unsigned long c;
		int spins = 0;

		/* A reader that announced a snapshot older than start may
		 * hold a pointer to something removed before the start. */
		while ((c = atomic_load_explicit(&r->ctr,
						 memory_order_acquire)) != 0 &&
		       seq_lt(c, start)) {
#ifdef URCU_BROKEN
			break;		/* deliberately wrong: don't wait */
#endif
			wait_a_little(spins++);
		}
	}
	pthread_mutex_unlock(&registry_lock);

	smp_mb();		/* readers' accesses before the end */
	atomic_store_explicit(&rcu_gp_seq, start + 1, memory_order_seq_cst);
	n_gp++;
}

/* Wait until the grace period recorded in snapshot s has completed. */
static void wait_for_gp(unsigned long s)
{
	pthread_mutex_lock(&gp_lock);
	n_gp_waits++;
	if (rcu_seq_done_val(atomic_load(&rcu_gp_seq), s))
		n_gp_shared++;		/* someone else's grace period did it */
	while (!rcu_seq_done_val(atomic_load(&rcu_gp_seq), s))
		run_one_gp();
	pthread_mutex_unlock(&gp_lock);
}

static unsigned long rcu_seq_snap(void)
{
	smp_mb();		/* the caller's removals before the snapshot */
	return rcu_seq_snap_val(atomic_load(&rcu_gp_seq));
}

void synchronize_rcu(void)
{
	wait_for_gp(rcu_seq_snap());
}

/* ---- callbacks -------------------------------------------------------- */

static pthread_mutex_t cb_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cb_work = PTHREAD_COND_INITIALIZER;
static pthread_cond_t cb_invoked = PTHREAD_COND_INITIALIZER;
static struct cblist cbs = {
	.head = NULL,
	.tails = { &cbs.head, &cbs.head, &cbs.head, &cbs.head },
};
static bool cb_thread_running, cb_stop;
static pthread_t cb_thread;
static unsigned long n_invoked, n_batches;

static void *cb_thread_fn(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&cb_lock);
	for (;;) {
		while (!cb_stop && !cblist_pending(&cbs))
			pthread_cond_wait(&cb_work, &cb_lock);
		if (!cblist_pending(&cbs))
			break;			/* stopping, nothing left */

		/* Number anything not numbered yet.  CB_WAIT is now
		 * non-empty: wait for the grace period it needs. */
		cblist_accelerate(&cbs, rcu_seq_snap());
		unsigned long need = cbs.gp_seq[CB_WAIT];

		pthread_mutex_unlock(&cb_lock);
		wait_for_gp(need);

		pthread_mutex_lock(&cb_lock);
		cblist_advance(&cbs, atomic_load(&rcu_gp_seq));
		long n;
		struct rcu_head *done = cblist_extract_done(&cbs, &n);

		pthread_mutex_unlock(&cb_lock);
		while (done) {			/* invoke outside the lock */
			struct rcu_head *next = done->next;

			done->func(done);
			done = next;
		}
		pthread_mutex_lock(&cb_lock);
		n_invoked += n;
		n_batches++;
		pthread_cond_broadcast(&cb_invoked);
	}
	pthread_mutex_unlock(&cb_lock);
	return NULL;
}

void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *head))
{
	head->func = func;
	pthread_mutex_lock(&cb_lock);
	if (!cb_thread_running) {
		if (pthread_create(&cb_thread, NULL, cb_thread_fn, NULL)) {
			perror("pthread_create");
			abort();
		}
		cb_thread_running = true;
	}
	cblist_enqueue(&cbs, head);
	cblist_accelerate(&cbs, rcu_seq_snap());
	pthread_cond_signal(&cb_work);
	pthread_mutex_unlock(&cb_lock);
}

/* ---- rcu_barrier() ---------------------------------------------------- */

struct barrier_cb {
	struct rcu_head rh;
	bool done;			/* under cb_lock */
};

static void barrier_func(struct rcu_head *rh)
{
	struct barrier_cb *b = container_of(rh, struct barrier_cb, rh);

	pthread_mutex_lock(&cb_lock);
	b->done = true;
	pthread_mutex_unlock(&cb_lock);
}

/*
 * Wait for every callback queued before this call to have been invoked.
 * Callbacks run in the order they were queued, so it is enough to queue
 * one more and wait for it, which is what the kernel does on each CPU.
 */
void rcu_barrier(void)
{
	struct barrier_cb b = { .done = false };

	call_rcu(&b.rh, barrier_func);
	pthread_mutex_lock(&cb_lock);
	while (!b.done)
		pthread_cond_wait(&cb_invoked, &cb_lock);
	pthread_mutex_unlock(&cb_lock);
}

void rcu_shutdown(void)
{
	pthread_mutex_lock(&cb_lock);
	bool running = cb_thread_running;

	cb_stop = true;
	pthread_cond_signal(&cb_work);
	pthread_mutex_unlock(&cb_lock);
	if (running)
		pthread_join(cb_thread, NULL);
	pthread_mutex_lock(&cb_lock);
	cb_thread_running = false;
	cb_stop = false;
	pthread_mutex_unlock(&cb_lock);
}

void rcu_get_stats(struct rcu_stats *st)
{
	pthread_mutex_lock(&gp_lock);
	st->grace_periods = n_gp;
	st->gp_waits = n_gp_waits;
	st->gp_shared = n_gp_shared;
	pthread_mutex_unlock(&gp_lock);
	pthread_mutex_lock(&cb_lock);
	st->callbacks = n_invoked;
	st->batches = n_batches;
	pthread_mutex_unlock(&cb_lock);
}
