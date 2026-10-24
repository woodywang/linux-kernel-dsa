// SPDX-License-Identifier: MIT
/*
 * pcpu_counter.h - a sharded counter in the style of the kernel's
 * percpu_counter (include/linux/percpu_counter.h, lib/percpu_counter.c).
 *
 * The counter is a shared 64-bit total plus one small signed delta per
 * shard.  In the kernel a shard is a CPU; here it is whatever index the
 * caller passes (the tests use one shard per thread).  An add changes only
 * the caller's shard, until the shard's delta would reach +/- batch; then
 * the delta is folded into the total under a lock.  So:
 *
 *   - pc_add() touches shared memory only once per ~batch units;
 *   - pc_read() returns the total alone: cheap, but it can be off by up to
 *     nr_shards * (batch - 1);
 *   - pc_sum() adds every shard's delta under the lock: exact when no add
 *     is running, slow in proportion to the number of shards.
 *
 * Each shard is written only by its owner and sits in its own cache line
 * (Chapter 1's false sharing).  Deltas are C11 atomics accessed with
 * relaxed ordering so that pc_sum() may read them while owners write them;
 * in the kernel these are plain per-CPU s32 variables.
 *
 * Deliberately left out: the kernel's protection against interrupts on the
 * same CPU (this_cpu_try_cmpxchg() or disabled interrupts), CPU hotplug
 * (folding a dead CPU's delta), percpu_counter_limited_add(), and the
 * dynamic default batch (max(32, 2 * online CPUs)).
 */
#ifndef PCPU_COUNTER_H
#define PCPU_COUNTER_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#define CACHELINE	64

struct shard {
	_Atomic int32_t delta;		/* written only by the owner */
} __attribute__((aligned(CACHELINE)));

struct pcpu_counter {
	pthread_mutex_t lock;		/* serializes folds and sums */
	_Atomic int64_t count;		/* the folded total */
	int32_t batch;
	int nr_shards;
	struct shard *shards;
};

static inline int pc_init(struct pcpu_counter *pc, int64_t value,
			  int nr_shards, int32_t batch)
{
	pc->shards = aligned_alloc(CACHELINE, nr_shards * sizeof(struct shard));
	if (!pc->shards)
		return -1;
	for (int i = 0; i < nr_shards; i++)
		atomic_init(&pc->shards[i].delta, 0);
	pthread_mutex_init(&pc->lock, NULL);
	atomic_init(&pc->count, value);
	pc->batch = batch;
	pc->nr_shards = nr_shards;
	return 0;
}

static inline void pc_destroy(struct pcpu_counter *pc)
{
	pthread_mutex_destroy(&pc->lock);
	free(pc->shards);
}

#define relaxed(p)	atomic_load_explicit(p, memory_order_relaxed)
#define set_relaxed(p, v) atomic_store_explicit(p, v, memory_order_relaxed)

static inline int64_t abs64(int64_t x)
{
	return x < 0 ? -x : x;
}

/* Add amount through shard me; fold into the total when the batch is hit. */
static inline void pc_add(struct pcpu_counter *pc, int me, int64_t amount)
{
	struct shard *s = &pc->shards[me];
	int64_t delta = relaxed(&s->delta);

	if (abs64(delta + amount) < pc->batch) {	/* fast path */
		set_relaxed(&s->delta, (int32_t)(delta + amount));
		return;
	}
	pthread_mutex_lock(&pc->lock);			/* fold */
	delta = relaxed(&s->delta);
	set_relaxed(&pc->count, relaxed(&pc->count) + delta + amount);
	set_relaxed(&s->delta, 0);
	pthread_mutex_unlock(&pc->lock);
}

/* The folded total only: fast, approximate. */
static inline int64_t pc_read(struct pcpu_counter *pc)
{
	return relaxed(&pc->count);
}

/* The total plus every shard's delta: exact if no add is in progress. */
static inline int64_t pc_sum(struct pcpu_counter *pc)
{
	int64_t sum;

	pthread_mutex_lock(&pc->lock);
	sum = relaxed(&pc->count);
	for (int i = 0; i < pc->nr_shards; i++)
		sum += relaxed(&pc->shards[i].delta);
	pthread_mutex_unlock(&pc->lock);
	return sum;
}

/* How far pc_read() can be from the true value. */
static inline int64_t pc_error_bound(const struct pcpu_counter *pc)
{
	return (int64_t)pc->nr_shards * (pc->batch - 1);
}

/*
 * Compare the counter with rhs: 1, 0 or -1.  Use the cheap read when it
 * is far enough from rhs that no hidden delta could change the answer,
 * as __percpu_counter_compare() does.
 */
static inline int pc_compare(struct pcpu_counter *pc, int64_t rhs)
{
	int64_t count = pc_read(pc);

	if (abs64(count - rhs) > pc_error_bound(pc))
		return count > rhs ? 1 : -1;
	count = pc_sum(pc);
	return (count > rhs) - (count < rhs);
}

#endif /* PCPU_COUNTER_H */
