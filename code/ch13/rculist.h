// SPDX-License-Identifier: MIT
/*
 * rculist.h - hash lists that readers may walk under rcu_read_lock().
 *
 * Teaching versions of the hlist and hlist_nulls operations in the
 * kernel's include/linux/rculist.h and rculist_nulls.h.  The structures
 * are those of Chapter 2; what changes is the order of the stores and how
 * they are marked:
 *
 *   - a new node is filled in completely, then published with one
 *     release store (rcu_assign_pointer);
 *   - a removed node keeps its ->next pointer, so that a reader standing
 *     on it can carry on; only ->pprev, which readers never use, is
 *     poisoned;
 *   - readers follow ->next with rcu_dereference() and never use ->pprev.
 *
 * One difference from the kernel: unlinking a node stores a pointer to its
 * successor, which readers could already reach, and the kernel does that
 * with WRITE_ONCE().  A reader that loads the pointer from that store has,
 * in the C11 memory model, no ordering with the successor's initialization
 * (which happened in some earlier writer), so here the unlink is a release
 * store too; ThreadSanitizer reports a race without it.
 *
 * Writers must exclude each other (a lock); readers need only
 * rcu_read_lock().  Freeing a removed node must wait for a grace period.
 */
#ifndef RCULIST_H
#define RCULIST_H

#include <stdint.h>
#include "urcu.h"

#define LIST_POISON2	((void *)0x122)		/* as in the kernel */

/* ---- hlist ------------------------------------------------------------ */

struct hlist_node {
	struct hlist_node *next, **pprev;
};

struct hlist_head {
	struct hlist_node *first;
};

#define hlist_entry_safe(ptr, type, member) ({				\
	typeof(ptr) ____p = (ptr);					\
	____p ? container_of(____p, type, member) : NULL; })

static inline void hlist_add_head_rcu(struct hlist_node *n,
				      struct hlist_head *h)
{
	struct hlist_node *first = h->first;

	n->next = first;		/* not yet visible to readers */
	n->pprev = &h->first;
	rcu_assign_pointer(h->first, n);	/* publish */
	if (first)
		first->pprev = &n->next;
}

static inline void hlist_del_rcu(struct hlist_node *n)
{
	struct hlist_node *next = n->next;

	/* Unlink: new readers skip n.  The kernel uses WRITE_ONCE() here;
	 * see the note on release ordering at the top of this file. */
	rcu_assign_pointer(*n->pprev, next);
	if (next)
		next->pprev = n->pprev;
	n->pprev = LIST_POISON2;	/* n->next stays valid */
}

/* Copy-update: put new in old's place with one published store. */
static inline void hlist_replace_rcu(struct hlist_node *old,
				     struct hlist_node *new)
{
	struct hlist_node *next = old->next;

	new->next = next;
	new->pprev = old->pprev;
	rcu_assign_pointer(*new->pprev, new);
	if (next)
		next->pprev = &new->next;
	old->pprev = LIST_POISON2;
}

#define hlist_for_each_entry_rcu(pos, head, member)			\
	for (pos = hlist_entry_safe(rcu_dereference((head)->first),	\
				    typeof(*(pos)), member);		\
	     pos;							\
	     pos = hlist_entry_safe(rcu_dereference((pos)->member.next), \
				    typeof(*(pos)), member))

/* ---- hlist_nulls ------------------------------------------------------ */

/* Chains end in an odd "nulls" value that names the chain (Chapter 2). */
struct hlist_nulls_node {
	struct hlist_nulls_node *next, **pprev;
};

struct hlist_nulls_head {
	struct hlist_nulls_node *first;
};

#define NULLS_MARKER(v)	((struct hlist_nulls_node *)(1UL | ((uintptr_t)(v) << 1)))

static inline bool is_a_nulls(const struct hlist_nulls_node *p)
{
	return (uintptr_t)p & 1;
}

static inline unsigned long get_nulls_value(const struct hlist_nulls_node *p)
{
	return (uintptr_t)p >> 1;
}

static inline void hlist_nulls_add_head_rcu(struct hlist_nulls_node *n,
					    struct hlist_nulls_head *h)
{
	struct hlist_nulls_node *first = h->first;

	WRITE_ONCE(n->next, first);	/* a reused node may still be read */
	n->pprev = &h->first;
	rcu_assign_pointer(h->first, n);
	if (!is_a_nulls(first))
		first->pprev = &n->next;
}

static inline void hlist_nulls_del_rcu(struct hlist_nulls_node *n)
{
	struct hlist_nulls_node *next = n->next;

	rcu_assign_pointer(*n->pprev, next);	/* kernel: WRITE_ONCE() */
	if (!is_a_nulls(next))
		next->pprev = n->pprev;
	n->pprev = LIST_POISON2;
}

#endif
