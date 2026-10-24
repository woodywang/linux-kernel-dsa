// SPDX-License-Identifier: MIT
/*
 * list_nulls.h - a teaching version of the kernel's hlist_nulls: an hlist
 * whose end is not NULL but a "nulls" marker that carries a value, usually
 * the number of the hash bucket the list belongs to.
 *
 * Nodes are at least 2-byte aligned, so a real node pointer always has its
 * low bit clear.  A marker has the low bit set and the value in the rest:
 *
 *     marker = (value << 1) | 1
 *
 * A lock-free reader that reaches the end of a chain can therefore tell
 * *which* chain it ended in (see nulls_race.c).
 *
 * What it leaves out on purpose: the RCU variants (the reason this list
 * exists; see Chapter 13) and the memory barriers they need.
 */
#ifndef LIST_NULLS_H
#define LIST_NULLS_H

#include <stdbool.h>
#include <stdint.h>
#include "list.h"

struct hlist_nulls_head {
	struct hlist_nulls_node *first;
};

struct hlist_nulls_node {
	struct hlist_nulls_node *next, **pprev;
};

#define NULLS_MARKER(value) \
	((struct hlist_nulls_node *)(1UL | ((uintptr_t)(value) << 1)))

static inline void INIT_HLIST_NULLS_HEAD(struct hlist_nulls_head *h,
					 unsigned long nulls)
{
	h->first = NULLS_MARKER(nulls);
}

static inline bool is_a_nulls(const struct hlist_nulls_node *ptr)
{
	return (uintptr_t)ptr & 1;
}

static inline unsigned long get_nulls_value(const struct hlist_nulls_node *ptr)
{
	return (uintptr_t)ptr >> 1;
}

static inline bool hlist_nulls_empty(const struct hlist_nulls_head *h)
{
	return is_a_nulls(h->first);
}

static inline bool hlist_nulls_unhashed(const struct hlist_nulls_node *n)
{
	return !n->pprev;
}

static inline void hlist_nulls_add_head(struct hlist_nulls_node *n,
					struct hlist_nulls_head *h)
{
	struct hlist_nulls_node *first = h->first;

	n->next = first;		/* may be the marker: it moves along */
	n->pprev = &h->first;
	h->first = n;
	if (!is_a_nulls(first))
		first->pprev = &n->next;
}

static inline void __hlist_nulls_del(struct hlist_nulls_node *n)
{
	struct hlist_nulls_node *next = n->next;
	struct hlist_nulls_node **pprev = n->pprev;

	*pprev = next;
	if (!is_a_nulls(next))
		next->pprev = pprev;
}

/* Like the kernel, leave ->next alone so a reader standing here can go on. */
static inline void hlist_nulls_del(struct hlist_nulls_node *n)
{
	__hlist_nulls_del(n);
	n->pprev = LIST_POISON2;
}

#define hlist_nulls_entry(ptr, type, member) container_of(ptr, type, member)

/* @pos ends on the marker, so the caller can check get_nulls_value(pos). */
#define hlist_nulls_for_each_entry(tpos, pos, head, member)		\
	for (pos = (head)->first;					\
	     !is_a_nulls(pos) &&					\
		({ tpos = hlist_nulls_entry(pos, typeof(*tpos), member); 1; }); \
	     pos = pos->next)

#endif
