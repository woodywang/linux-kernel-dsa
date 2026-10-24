// SPDX-License-Identifier: MIT
/*
 * ilist.h - a minimal intrusive, circular, doubly linked list in the style
 * of the kernel's list_head.  The list node lives *inside* the object; an
 * object can sit on several lists at once by embedding several nodes.
 */
#ifndef ILIST_H
#define ILIST_H

#include <stdbool.h>
#include "container_of.h"

struct list_head {
	struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }

static inline void list_init(struct list_head *h)
{
	h->next = h->prev = h;
}

static inline bool list_empty(const struct list_head *h)
{
	return h->next == h;
}

/* Insert @n between two known neighbours. */
static inline void __list_add(struct list_head *n,
			      struct list_head *prev, struct list_head *next)
{
	next->prev = n;
	n->next = next;
	n->prev = prev;
	prev->next = n;
}

static inline void list_add(struct list_head *n, struct list_head *head)
{
	__list_add(n, head, head->next);	/* at the front */
}

static inline void list_add_tail(struct list_head *n, struct list_head *head)
{
	__list_add(n, head->prev, head);	/* at the back */
}

/* O(1): the node knows its neighbours, so no search is needed. */
static inline void list_del(struct list_head *n)
{
	n->prev->next = n->next;
	n->next->prev = n->prev;
	list_init(n);
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)

/* Visit every object on the list; @pos is a pointer to the object type. */
#define list_for_each_entry(pos, head, member)				\
	for (pos = list_entry((head)->next, typeof(*pos), member);	\
	     &pos->member != (head);					\
	     pos = list_entry(pos->member.next, typeof(*pos), member))

/* Same, but @pos may be removed from the list inside the loop. */
#define list_for_each_entry_safe(pos, tmp, head, member)		\
	for (pos = list_entry((head)->next, typeof(*pos), member),	\
	     tmp = list_entry(pos->member.next, typeof(*pos), member);	\
	     &pos->member != (head);					\
	     pos = tmp,							\
	     tmp = list_entry(tmp->member.next, typeof(*tmp), member))

#endif
