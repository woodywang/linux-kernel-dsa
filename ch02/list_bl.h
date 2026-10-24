// SPDX-License-Identifier: MIT
/*
 * list_bl.h - a teaching version of the kernel's hlist_bl: an hlist whose
 * head pointer doubles as a spinlock.  Bit 0 of ->first is the lock; the
 * rest is the pointer to the first node (nodes are aligned, so a real
 * pointer has bit 0 clear).  A hash table gets one lock per bucket at no
 * cost in memory.
 *
 * Rules, as in the kernel:
 *   - Modify a chain only while holding its lock.
 *   - Every write of ->first must keep the lock bit, and every read must
 *     mask it off (hlist_bl_first()).
 *
 * The lock uses GCC/Clang __atomic builtins: compare-and-swap to take it
 * (acquire ordering), a plain store to drop it (release ordering).  All
 * stores to a word that another thread may be spinning on are atomic.
 *
 * What it leaves out on purpose: the RCU variants, lock debugging, and the
 * kernel's uniprocessor build, in which the lock bit disappears.
 */
#ifndef LIST_BL_H
#define LIST_BL_H

#include <stdbool.h>
#include <stdint.h>
#include "list.h"

#define LIST_BL_LOCKMASK	1UL

struct hlist_bl_head {
	struct hlist_bl_node *first;	/* pointer | lock bit */
};

struct hlist_bl_node {
	struct hlist_bl_node *next, **pprev;
};

#define bl_ptr(word)	((struct hlist_bl_node *)(word))

static inline uintptr_t bl_load(struct hlist_bl_node *const *p)
{
	return (uintptr_t)__atomic_load_n(p, __ATOMIC_RELAXED);
}

static inline void bl_store(struct hlist_bl_node **p, uintptr_t v)
{
	__atomic_store_n(p, bl_ptr(v), __ATOMIC_RELAXED);
}

static inline void INIT_HLIST_BL_HEAD(struct hlist_bl_head *h)
{
	h->first = NULL;
}

static inline void INIT_HLIST_BL_NODE(struct hlist_bl_node *n)
{
	n->next = NULL;
	n->pprev = NULL;
}

static inline bool hlist_bl_unhashed(const struct hlist_bl_node *n)
{
	return !n->pprev;
}

/* Spin until bit 0 was clear and we were the one to set it. */
static inline void hlist_bl_lock(struct hlist_bl_head *b)
{
	for (;;) {
		struct hlist_bl_node *old = bl_ptr(bl_load(&b->first) &
						   ~LIST_BL_LOCKMASK);
		struct hlist_bl_node *locked =
			bl_ptr((uintptr_t)old | LIST_BL_LOCKMASK);

		if (__atomic_compare_exchange_n(&b->first, &old, locked, false,
						__ATOMIC_ACQUIRE,
						__ATOMIC_RELAXED))
			return;
		while (bl_load(&b->first) & LIST_BL_LOCKMASK)
			;			/* spin on a plain load */
	}
}

static inline void hlist_bl_unlock(struct hlist_bl_head *b)
{
	__atomic_store_n(&b->first,
			 bl_ptr(bl_load(&b->first) & ~LIST_BL_LOCKMASK),
			 __ATOMIC_RELEASE);
}

static inline bool hlist_bl_is_locked(struct hlist_bl_head *b)
{
	return bl_load(&b->first) & LIST_BL_LOCKMASK;
}

/* The first node, with the lock bit masked off. */
static inline struct hlist_bl_node *hlist_bl_first(struct hlist_bl_head *h)
{
	return bl_ptr(bl_load(&h->first) & ~LIST_BL_LOCKMASK);
}

static inline bool hlist_bl_empty(struct hlist_bl_head *h)
{
	return !hlist_bl_first(h);
}

/* Caller holds the lock, so the new value keeps bit 0 set. */
static inline void hlist_bl_set_first(struct hlist_bl_head *h,
				      struct hlist_bl_node *n)
{
	bl_store(&h->first, (uintptr_t)n | LIST_BL_LOCKMASK);
}

static inline void hlist_bl_add_head(struct hlist_bl_node *n,
				     struct hlist_bl_head *h)
{
	struct hlist_bl_node *first = hlist_bl_first(h);

	n->next = first;
	if (first)
		first->pprev = &n->next;
	n->pprev = &h->first;
	hlist_bl_set_first(h, n);
}

static inline void __hlist_bl_del(struct hlist_bl_node *n)
{
	struct hlist_bl_node *next = n->next;
	struct hlist_bl_node **pprev = n->pprev;

	/* pprev may point at the head's ->first: keep whatever lock bit is there */
	bl_store(pprev, (uintptr_t)next | (bl_load(pprev) & LIST_BL_LOCKMASK));
	if (next)
		next->pprev = pprev;
}

static inline void hlist_bl_del(struct hlist_bl_node *n)
{
	__hlist_bl_del(n);
	n->next = LIST_POISON1;
	n->pprev = LIST_POISON2;
}

static inline void hlist_bl_del_init(struct hlist_bl_node *n)
{
	if (!hlist_bl_unhashed(n)) {
		__hlist_bl_del(n);
		INIT_HLIST_BL_NODE(n);
	}
}

#define hlist_bl_entry(ptr, type, member) container_of(ptr, type, member)

#define hlist_bl_for_each_entry(tpos, pos, head, member)		\
	for (pos = hlist_bl_first(head);				\
	     pos &&							\
		({ tpos = hlist_bl_entry(pos, typeof(*tpos), member); 1; }); \
	     pos = pos->next)

#endif
