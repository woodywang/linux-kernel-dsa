// SPDX-License-Identifier: MIT
/*
 * cblist.h - a segmented callback list and grace-period sequence numbers.
 *
 * Follows the design of the kernel's rcu_segcblist (kernel/rcu/
 * rcu_segcblist.c) and the rcu_seq_*() helpers in kernel/rcu/rcu.h:
 *
 *   - A grace-period sequence number keeps a counter in its upper bits and
 *     the state "a grace period is in progress" in its low bit.  (The
 *     kernel reserves two low bits; one is enough here.)
 *   - All callbacks waiting on one CPU (here: one list) form a single
 *     singly linked list, cut into four segments by four tail pointers.
 *     Each numbered segment records the grace period its callbacks need.
 *     Moving callbacks from one segment to the next moves a tail pointer,
 *     not the callbacks.
 *
 * Deliberately left out: per-segment lengths, the offloaded ("nocb")
 * variants and their locking, expedited grace periods, and lazy
 * callbacks.  The caller provides all locking.
 */
#ifndef CBLIST_H
#define CBLIST_H

#include <stdbool.h>
#include <stddef.h>

struct rcu_head {
	struct rcu_head *next;
	void (*func)(struct rcu_head *head);
};

/* ---- grace-period sequence numbers ---------------------------------- */

#define RCU_SEQ_STATE_MASK	1UL	/* low bit: grace period in progress */

/* Wrap-safe comparison of two sequence numbers. */
static inline bool seq_lt(unsigned long a, unsigned long b)
{
	return (long)(a - b) < 0;
}

/*
 * The sequence number at which a full grace period will have elapsed
 * since now: the end of the next grace period if none is running, or of
 * the one after the current one if one is.
 */
static inline unsigned long rcu_seq_snap_val(unsigned long cur)
{
	return (cur + 2 * RCU_SEQ_STATE_MASK + 1) & ~RCU_SEQ_STATE_MASK;
}

/* Has the grace period that snapshot s waits for completed? */
static inline bool rcu_seq_done_val(unsigned long cur, unsigned long s)
{
	return !seq_lt(cur, s);
}

/* ---- the segmented list --------------------------------------------- */

enum {
	CB_DONE,	/* grace period over: ready to invoke */
	CB_WAIT,	/* waiting for grace period gp_seq[CB_WAIT] */
	CB_NEXT_READY,	/* waiting for a later one, gp_seq[CB_NEXT_READY] */
	CB_NEXT,	/* not yet assigned a grace period */
	CB_NSEGS
};

struct cblist {
	struct rcu_head *head;
	struct rcu_head **tails[CB_NSEGS];	/* tails[i]: end of segment i */
	unsigned long gp_seq[CB_NSEGS];		/* for CB_WAIT, CB_NEXT_READY */
	long len;
};

static inline void cblist_init(struct cblist *l)
{
	l->head = NULL;
	for (int i = 0; i < CB_NSEGS; i++) {
		l->tails[i] = &l->head;
		l->gp_seq[i] = 0;
	}
	l->len = 0;
}

/* Segment i runs from the end of segment i - 1 to tails[i]. */
static inline bool cblist_segempty(const struct cblist *l, int i)
{
	if (i == CB_DONE)
		return l->tails[CB_DONE] == &l->head;
	return l->tails[i] == l->tails[i - 1];
}

/* Are any callbacks still waiting for a grace period? */
static inline bool cblist_pending(const struct cblist *l)
{
	return l->tails[CB_NEXT] != l->tails[CB_DONE];
}

/* Append to the unnumbered segment: O(1), no search. */
static inline void cblist_enqueue(struct cblist *l, struct rcu_head *rhp)
{
	rhp->next = NULL;
	*l->tails[CB_NEXT] = rhp;
	l->tails[CB_NEXT] = &rhp->next;
	l->len++;
}

/*
 * Give the callbacks in CB_NEXT the grace-period number seq: merge them
 * into the newest numbered segment if it needs the same grace period,
 * otherwise start a new numbered segment.  Returns false if both
 * numbered segments are taken by earlier grace periods; the callbacks
 * then stay unnumbered until cblist_advance() frees a segment.
 */
static inline bool cblist_accelerate(struct cblist *l, unsigned long seq)
{
	int i = CB_NEXT_READY;

	if (cblist_segempty(l, CB_NEXT))
		return false;
	while (i > CB_DONE && cblist_segempty(l, i))
		i--;				/* newest non-empty segment */
	if (i == CB_DONE || l->gp_seq[i] != seq)
		i++;				/* need a segment of its own */
	if (i == CB_NEXT)
		return false;
	for (int j = i; j < CB_NEXT; j++)
		l->tails[j] = l->tails[CB_NEXT];
	l->gp_seq[i] = seq;
	return true;
}

/*
 * Grace period number done_seq has completed: move every numbered
 * segment that was waiting for it (or an earlier one) into CB_DONE, and
 * slide the remaining numbered segment down so that CB_WAIT is never
 * empty while CB_NEXT_READY is not.
 */
static inline void cblist_advance(struct cblist *l, unsigned long done_seq)
{
	int i, j;

	for (i = CB_WAIT; i < CB_NEXT; i++) {
		if (cblist_segempty(l, i) || seq_lt(done_seq, l->gp_seq[i]))
			break;
		l->tails[CB_DONE] = l->tails[i];
	}
	for (j = CB_WAIT; i < CB_NEXT; i++, j++) {
		l->tails[j] = l->tails[i];
		l->gp_seq[j] = l->gp_seq[i];
	}
	for (; j < CB_NEXT; j++)
		l->tails[j] = l->tails[j - 1];	/* now empty */
}

/*
 * Detach the CB_DONE segment and return it as a NULL-terminated list;
 * *n receives its length.
 */
static inline struct rcu_head *cblist_extract_done(struct cblist *l, long *n)
{
	struct rcu_head *first = l->head, **done_tail = l->tails[CB_DONE];
	long cnt = 0;

	if (done_tail == &l->head) {
		*n = 0;
		return NULL;
	}
	l->head = *done_tail;
	*done_tail = NULL;
	for (int i = CB_NSEGS - 1; i >= CB_DONE; i--)
		if (l->tails[i] == done_tail)
			l->tails[i] = &l->head;
	for (struct rcu_head *p = first; p; p = p->next)
		cnt++;
	l->len -= cnt;
	*n = cnt;
	return first;
}

#endif
