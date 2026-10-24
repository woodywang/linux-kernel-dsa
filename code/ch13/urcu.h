// SPDX-License-Identifier: MIT
/*
 * urcu.h - a small userspace RCU written for this book.
 *
 * The read side follows the counter-based design of the userspace RCU
 * library (liburcu) when the membarrier() system call is not available
 * (its "urcu-mb" scheme): each registered reader thread publishes, on
 * entry to its outermost read-side critical section, a snapshot of the
 * global grace-period sequence number, and clears it on exit.  A grace
 * period starts by advancing the sequence number and ends when no reader
 * still shows a snapshot older than the start.
 *
 * The update side provides synchronize_rcu(), call_rcu() with batching
 * through the segmented callback list of cblist.h, and rcu_barrier().
 * Callbacks run on one background thread, the analogue of the kernel's
 * callback processing.
 *
 * Memory ordering is expressed with C11 atomics; smp_mb() is a sequentially
 * consistent fence.  ThreadSanitizer does not model fences, so under
 * -fsanitize=thread smp_mb() becomes an atomic read-modify-write of one
 * shared variable, which also orders the pattern used here but makes every
 * reader write a shared cache line: correct, slow, and only for testing.
 *
 * Deliberately left out: per-CPU anything, the kernel's rcu_node tree,
 * expedited and polled grace periods, priority boosting, CPU hotplug,
 * stall warnings, and the membarrier() trick that lets liburcu drop the
 * reader's full barrier.  Readers must call rcu_register_thread() first.
 */
#ifndef URCU_H
#define URCU_H

#include <stdatomic.h>
#include <stdbool.h>
#include "cblist.h"
#include "container_of.h"

/* ---- marked accesses ------------------------------------------------- */

#define READ_ONCE(x)		__atomic_load_n(&(x), __ATOMIC_RELAXED)
#define WRITE_ONCE(x, v)	__atomic_store_n(&(x), (v), __ATOMIC_RELAXED)

#if defined(__SANITIZE_THREAD__)
#define URCU_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define URCU_TSAN 1
#endif
#endif

#ifdef URCU_TSAN
extern _Atomic unsigned long urcu_tsan_fence;
#define smp_mb()	atomic_fetch_add_explicit(&urcu_tsan_fence, 0, \
						  memory_order_acq_rel)
#else
#define smp_mb()	atomic_thread_fence(memory_order_seq_cst)
#endif

/* ---- publish and subscribe ------------------------------------------- */

/* Publish p = v: everything written to *v before this is visible to a
 * reader that sees the new pointer.  A release store. */
#define rcu_assign_pointer(p, v) \
	__atomic_store_n(&(p), (v), __ATOMIC_RELEASE)

/* Load a pointer published with rcu_assign_pointer().  The kernel relies
 * on the address dependency ("consume"); C compilers implement consume as
 * acquire, so this is an acquire load. */
#define rcu_dereference(p) \
	__atomic_load_n(&(p), __ATOMIC_ACQUIRE)

/* ---- readers ---------------------------------------------------------- */

struct rcu_reader {
	_Atomic unsigned long ctr;	/* 0, or gp_seq at outermost lock */
	unsigned int nesting;		/* touched only by the owner */
	bool registered;
	struct rcu_reader *next;	/* registry, under the registry lock */
};

extern _Thread_local struct rcu_reader rcu_me;
extern _Atomic unsigned long rcu_gp_seq;

void rcu_register_thread(void);
void rcu_unregister_thread(void);

static inline void rcu_read_lock(void)
{
	if (rcu_me.nesting++ == 0) {
		unsigned long gp = atomic_load_explicit(&rcu_gp_seq,
							memory_order_acquire);

		/* Announce "reading since gp", then order the announcement
		 * before every load in the critical section. */
		atomic_store_explicit(&rcu_me.ctr, gp, memory_order_release);
		smp_mb();
	}
}

static inline void rcu_read_unlock(void)
{
	if (--rcu_me.nesting == 0)	/* every load above happens before this */
		atomic_store_explicit(&rcu_me.ctr, 0, memory_order_release);
}

/* ---- updaters --------------------------------------------------------- */

void synchronize_rcu(void);
void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *head));
void rcu_barrier(void);

/* Start and stop the callback thread (call_rcu() starts it on demand). */
void rcu_shutdown(void);

struct rcu_stats {
	unsigned long grace_periods;	/* grace periods completed */
	unsigned long gp_waits;		/* waits for a grace period */
	unsigned long gp_shared;	/* ... that needed none of their own */
	unsigned long callbacks;	/* callbacks invoked */
	unsigned long batches;		/* times the callback thread ran */
};
void rcu_get_stats(struct rcu_stats *st);

#endif
