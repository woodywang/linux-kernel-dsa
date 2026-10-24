// SPDX-License-Identifier: MIT
/*
 * hlist.h - a minimal intrusive hash-bucket list in the style of the
 * kernel's hlist_head / hlist_node (Chapter 2 covers the real one).
 *
 * The head is a single pointer, so a bucket array costs one word per
 * bucket.  A node keeps a pointer to the previous node's "next" field
 * (pprev) instead of a pointer to the previous node, so it can unlink
 * itself in O(1) without knowing whether it is first in its chain.
 */
#ifndef HLIST_H
#define HLIST_H

#include <stdbool.h>
#include <stddef.h>
#include "container_of.h"

struct hlist_head {
	struct hlist_node *first;
};

struct hlist_node {
	struct hlist_node *next, **pprev;
};

#define HLIST_HEAD_INIT { .first = NULL }

static inline void INIT_HLIST_NODE(struct hlist_node *n)
{
	n->next = NULL;
	n->pprev = NULL;
}

static inline bool hlist_unhashed(const struct hlist_node *n)
{
	return !n->pprev;
}

static inline bool hlist_empty(const struct hlist_head *h)
{
	return !h->first;
}

static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
	struct hlist_node *first = h->first;

	n->next = first;
	if (first)
		first->pprev = &n->next;
	h->first = n;
	n->pprev = &h->first;
}

static inline void hlist_del_init(struct hlist_node *n)
{
	if (hlist_unhashed(n))
		return;
	*n->pprev = n->next;
	if (n->next)
		n->next->pprev = n->pprev;
	INIT_HLIST_NODE(n);
}

#define hlist_entry_safe(ptr, type, member) ({				\
	typeof(ptr) ____ptr = (ptr);					\
	____ptr ? container_of(____ptr, type, member) : NULL; })

#define hlist_for_each_entry(pos, head, member)				\
	for (pos = hlist_entry_safe((head)->first, typeof(*(pos)), member); \
	     pos;							\
	     pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member))

#endif
