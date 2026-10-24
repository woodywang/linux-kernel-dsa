// SPDX-License-Identifier: MIT
/*
 * seqcount.h - sequence counters and sequential locks in the style of the
 * kernel's include/linux/seqlock.h (seqcount_t and seqlock_t).
 *
 * A writer makes the counter odd, changes the data, and makes it even
 * again.  A reader records the (even) counter, copies the data out, and
 * retries if the counter changed meanwhile.  Readers never write shared
 * memory and never block the writer.
 *
 * Memory ordering.  The kernel orders the counter and the data with
 * smp_wmb() in the writer and smp_rmb() in the reader, and accesses the
 * data with plain loads and stores.  In C11 a plain access that races with
 * a write is undefined behaviour, so here every protected field is an
 * atomic accessed through seq_store() / seq_load(), and the ordering is
 * carried by those accesses instead of by fences:
 *
 *   writer: counter = odd (relaxed); data stores (release, so none of
 *           them can become visible before the odd counter); counter =
 *           even (release, so it is visible only after all of them);
 *   reader: counter (acquire); data loads (acquire, so the final counter
 *           load cannot be performed before any of them); counter again.
 *
 * (Standalone fences would be cheaper on some machines; this version
 * keeps every ordering on an access, which ThreadSanitizer checks.)
 *
 * Deliberately left out: lockdep, the seqcount_LOCKNAME_t variants, the
 * PREEMPT_RT lock-then-unlock trick, and the _irq/_bh writer variants.
 */
#ifndef SEQCOUNT_H
#define SEQCOUNT_H

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>

#define seq_store(p, v)	atomic_store_explicit(p, v, memory_order_release)
#define seq_load(p)	atomic_load_explicit(p, memory_order_acquire)

struct seqcount {
	_Atomic unsigned sequence;
};

static inline void seqcount_init(struct seqcount *s)
{
	atomic_init(&s->sequence, 0);
}

/* The counter as it is, odd or even (for tests and demos). */
static inline unsigned raw_read_seqcount(const struct seqcount *s)
{
	return atomic_load_explicit(&s->sequence, memory_order_acquire);
}

static inline unsigned read_seqcount_begin(const struct seqcount *s)
{
	unsigned seq;

	while ((seq = raw_read_seqcount(s)) & 1)
		sched_yield();		/* a writer is active: wait for it */
	return seq;
}

static inline bool read_seqcount_retry(const struct seqcount *s,
				       unsigned start)
{
	return atomic_load_explicit(&s->sequence, memory_order_relaxed) != start;
}

#ifdef SEQ_NO_RETRY	/* deliberately broken (Exercise 14.4) */
#define read_seqcount_retry(s, start)	((void)(s), (void)(start), false)
#endif

/* Writers must be serialized by the caller. */
static inline void write_seqcount_begin(struct seqcount *s)
{
	unsigned seq = atomic_load_explicit(&s->sequence, memory_order_relaxed);

	atomic_store_explicit(&s->sequence, seq + 1, memory_order_relaxed);
}

static inline void write_seqcount_end(struct seqcount *s)
{
	unsigned seq = atomic_load_explicit(&s->sequence, memory_order_relaxed);

	atomic_store_explicit(&s->sequence, seq + 1, memory_order_release);
}

/* seqlock_t: a seqcount plus the lock that serializes its writers. */
struct seqlock {
	struct seqcount seqcount;
	pthread_mutex_t lock;
};

static inline void seqlock_init(struct seqlock *sl)
{
	seqcount_init(&sl->seqcount);
	pthread_mutex_init(&sl->lock, NULL);
}

static inline void write_seqlock(struct seqlock *sl)
{
	pthread_mutex_lock(&sl->lock);
	write_seqcount_begin(&sl->seqcount);
}

static inline void write_sequnlock(struct seqlock *sl)
{
	write_seqcount_end(&sl->seqcount);
	pthread_mutex_unlock(&sl->lock);
}

static inline unsigned read_seqbegin(const struct seqlock *sl)
{
	return read_seqcount_begin(&sl->seqcount);
}

static inline bool read_seqretry(const struct seqlock *sl, unsigned start)
{
	return read_seqcount_retry(&sl->seqcount, start);
}

#endif /* SEQCOUNT_H */
