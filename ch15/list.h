// SPDX-License-Identifier: MIT
/*
 * list.h - a teaching version of the kernel's intrusive lists: the circular
 * doubly linked list (struct list_head) and the hash-bucket list with a
 * one-pointer head (struct hlist_head / struct hlist_node).
 *
 * It follows the kernel's API and invariants closely:
 *   - list_del() poisons the removed node; list_del_init() re-initializes it.
 *   - With LIST_HARDENED defined, every insertion and deletion first checks
 *     that its neighbours still point at each other.  A failed check is
 *     reported and the operation is skipped, as in the kernel.
 *
 * What it leaves out on purpose: READ_ONCE()/WRITE_ONCE() and memory
 * barriers (this code is for one thread at a time; see list_bl.h for a
 * lock), the RCU variants, and most of the kernel's iterator variants.
 * Chapter 1's ilist.h is the smaller version this file grows from; note
 * that its list_del() re-initializes, which the kernel calls list_del_init().
 */
#ifndef LIST_H
#define LIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "container_of.h"

struct list_head {
	struct list_head *next, *prev;
};

struct hlist_head {
	struct hlist_node *first;
};

struct hlist_node {
	struct hlist_node *next, **pprev;
};

/*
 * Poison values: non-NULL pointers that fault when dereferenced.  On 64-bit
 * machines the kernel adds an offset that puts them in a range no program
 * can map (0xdead000000000000 on x86-64 and arm64).
 */
#if UINTPTR_MAX > 0xffffffffUL
#define POISON_POINTER_DELTA	0xdead000000000000UL
#else
#define POISON_POINTER_DELTA	0UL
#endif
#define LIST_POISON1	((void *)(0x100 + POISON_POINTER_DELTA))
#define LIST_POISON2	((void *)(0x122 + POISON_POINTER_DELTA))

/* ------------------------------------------------------------------ */
/* Corruption checks (the kernel's CONFIG_LIST_HARDENED / DEBUG_LIST)  */

static inline long *list_corruptions(void)
{
	static long count;
	return &count;
}

/* Report a broken invariant; the caller then skips the operation. */
static inline bool list_corrupt(const char *what)
{
	++*list_corruptions();
	printf("  caught: %s\n", what);
	return false;
}

static inline bool __list_add_valid(struct list_head *new,
				    struct list_head *prev,
				    struct list_head *next)
{
#ifdef LIST_HARDENED
	if (next->prev != prev)
		return list_corrupt("list_add corruption: next->prev != prev");
	if (prev->next != next)
		return list_corrupt("list_add corruption: prev->next != next");
	if (new == prev || new == next)
		return list_corrupt("list_add double add");
#else
	(void)new, (void)prev, (void)next;
#endif
	return true;
}

static inline bool __list_del_entry_valid(struct list_head *entry)
{
#ifdef LIST_HARDENED
	struct list_head *prev = entry->prev, *next = entry->next;

	if (next == LIST_POISON1 || prev == LIST_POISON2)
		return list_corrupt("list_del corruption: entry is poisoned");
	if (prev->next != entry)
		return list_corrupt("list_del corruption: prev->next != entry");
	if (next->prev != entry)
		return list_corrupt("list_del corruption: next->prev != entry");
#else
	(void)entry;
#endif
	return true;
}

/* ------------------------------------------------------------------ */
/* The circular doubly linked list                                     */

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
	list->next = list;
	list->prev = list;
}

/* The two primitives.  Everything else is built from these. */
static inline void __list_add(struct list_head *new,
			      struct list_head *prev, struct list_head *next)
{
	if (!__list_add_valid(new, prev, next))
		return;
	next->prev = new;
	new->next = next;
	new->prev = prev;
	prev->next = new;
}

static inline void __list_del(struct list_head *prev, struct list_head *next)
{
	next->prev = prev;
	prev->next = next;
}

static inline void list_add(struct list_head *new, struct list_head *head)
{
	__list_add(new, head, head->next);	/* after head: a stack */
}

static inline void list_add_tail(struct list_head *new, struct list_head *head)
{
	__list_add(new, head->prev, head);	/* before head: a queue */
}

static inline void __list_del_entry(struct list_head *entry)
{
	if (!__list_del_entry_valid(entry))
		return;
	__list_del(entry->prev, entry->next);
}

/* Unlink and poison: any later use of the node's links will fault. */
static inline void list_del(struct list_head *entry)
{
	__list_del_entry(entry);
	entry->next = LIST_POISON1;
	entry->prev = LIST_POISON2;
}

/* Unlink and make the node an empty list: safe to test or delete again. */
static inline void list_del_init(struct list_head *entry)
{
	__list_del_entry(entry);
	INIT_LIST_HEAD(entry);
}

static inline bool list_empty(const struct list_head *head)
{
	return head->next == head;
}

static inline bool list_is_singular(const struct list_head *head)
{
	return !list_empty(head) && head->next == head->prev;
}

static inline bool list_is_first(const struct list_head *list,
				 const struct list_head *head)
{
	return list->prev == head;
}

static inline bool list_is_last(const struct list_head *list,
				const struct list_head *head)
{
	return list->next == head;
}

/* Put @new where @old was.  @old's own links are left stale. */
static inline void list_replace(struct list_head *old, struct list_head *new)
{
	new->next = old->next;
	new->next->prev = new;
	new->prev = old->prev;
	new->prev->next = new;
}

/* Exchange the positions of two entries, possibly on different lists. */
static inline void list_swap(struct list_head *entry1,
			     struct list_head *entry2)
{
	struct list_head *pos = entry2->prev;

	list_del(entry2);
	list_replace(entry1, entry2);
	if (pos == entry1)
		pos = entry2;
	list_add(entry1, pos);
}

static inline void list_move(struct list_head *list, struct list_head *head)
{
	__list_del_entry(list);
	list_add(list, head);
}

static inline void list_move_tail(struct list_head *list,
				  struct list_head *head)
{
	__list_del_entry(list);
	list_add_tail(list, head);
}

/* Move the run first..last (inclusive) of one list to just before @head. */
static inline void list_bulk_move_tail(struct list_head *head,
				       struct list_head *first,
				       struct list_head *last)
{
	first->prev->next = last->next;		/* close the gap */
	last->next->prev = first->prev;

	head->prev->next = first;		/* link in before head */
	first->prev = head->prev;
	last->next = head;
	head->prev = last;
}

/* The head is a node like any other: moving it rotates the list. */
static inline void list_rotate_left(struct list_head *head)
{
	if (!list_empty(head))
		list_move_tail(head->next, head);
}

static inline void list_rotate_to_front(struct list_head *list,
					struct list_head *head)
{
	list_move_tail(head, list);
}

/* Insert all of @list's entries between @prev and @next. */
static inline void __list_splice(const struct list_head *list,
				 struct list_head *prev,
				 struct list_head *next)
{
	struct list_head *first = list->next;
	struct list_head *last = list->prev;

	first->prev = prev;
	prev->next = first;
	last->next = next;
	next->prev = last;
}

static inline void list_splice(const struct list_head *list,
			       struct list_head *head)
{
	if (!list_empty(list))
		__list_splice(list, head, head->next);
}

static inline void list_splice_tail(const struct list_head *list,
				    struct list_head *head)
{
	if (!list_empty(list))
		__list_splice(list, head->prev, head);
}

/* Splice, then re-initialize @list so that it is empty and reusable. */
static inline void list_splice_init(struct list_head *list,
				    struct list_head *head)
{
	if (!list_empty(list)) {
		__list_splice(list, head, head->next);
		INIT_LIST_HEAD(list);
	}
}

static inline void list_splice_tail_init(struct list_head *list,
					 struct list_head *head)
{
	if (!list_empty(list)) {
		__list_splice(list, head->prev, head);
		INIT_LIST_HEAD(list);
	}
}

/* Move head's entries up to and including @entry onto @list. */
static inline void __list_cut_position(struct list_head *list,
				       struct list_head *head,
				       struct list_head *entry)
{
	struct list_head *new_first = entry->next;

	list->next = head->next;
	list->next->prev = list;
	list->prev = entry;
	entry->next = list;
	head->next = new_first;
	new_first->prev = head;
}

static inline void list_cut_position(struct list_head *list,
				     struct list_head *head,
				     struct list_head *entry)
{
	if (list_empty(head))
		return;
	if (entry == head)
		INIT_LIST_HEAD(list);
	else
		__list_cut_position(list, head, entry);
}

/* Move head's entries before (not including) @entry onto @list. */
static inline void list_cut_before(struct list_head *list,
				   struct list_head *head,
				   struct list_head *entry)
{
	if (head->next == entry) {
		INIT_LIST_HEAD(list);
		return;
	}
	list->next = head->next;
	list->next->prev = list;
	list->prev = entry->prev;
	list->prev->next = list;
	head->next = entry;
	entry->prev = head;
}

#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(head, type, member) \
	list_entry((head)->next, type, member)
#define list_last_entry(head, type, member) \
	list_entry((head)->prev, type, member)
#define list_next_entry(pos, member) \
	list_entry((pos)->member.next, typeof(*(pos)), member)
#define list_prev_entry(pos, member) \
	list_entry((pos)->member.prev, typeof(*(pos)), member)

#define list_for_each(pos, head) \
	for (pos = (head)->next; pos != (head); pos = pos->next)

#define list_for_each_entry(pos, head, member)				\
	for (pos = list_first_entry(head, typeof(*pos), member);	\
	     &pos->member != (head);					\
	     pos = list_next_entry(pos, member))

#define list_for_each_entry_reverse(pos, head, member)			\
	for (pos = list_last_entry(head, typeof(*pos), member);		\
	     &pos->member != (head);					\
	     pos = list_prev_entry(pos, member))

/* @n is fetched before the body runs, so the body may delete @pos. */
#define list_for_each_entry_safe(pos, n, head, member)			\
	for (pos = list_first_entry(head, typeof(*pos), member),	\
	     n = list_next_entry(pos, member);				\
	     &pos->member != (head);					\
	     pos = n, n = list_next_entry(n, member))

static inline size_t list_count_nodes(const struct list_head *head)
{
	const struct list_head *pos;
	size_t count = 0;

	list_for_each(pos, head)
		count++;
	return count;
}

/* ------------------------------------------------------------------ */
/* hlist: a NULL-terminated list whose head is a single pointer        */

#define HLIST_HEAD_INIT { .first = NULL }

static inline void INIT_HLIST_HEAD(struct hlist_head *h)
{
	h->first = NULL;
}

static inline void INIT_HLIST_NODE(struct hlist_node *n)
{
	n->next = NULL;
	n->pprev = NULL;
}

/* A node that is on no list has no "previous next pointer". */
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

/* @next must be on a list; @n goes in front of it. */
static inline void hlist_add_before(struct hlist_node *n,
				    struct hlist_node *next)
{
	n->pprev = next->pprev;
	n->next = next;
	next->pprev = &n->next;
	*(n->pprev) = n;
}

/* @prev must be on a list; @n goes after it. */
static inline void hlist_add_behind(struct hlist_node *n,
				    struct hlist_node *prev)
{
	n->next = prev->next;
	prev->next = n;
	n->pprev = &prev->next;
	if (n->next)
		n->next->pprev = &n->next;
}

/*
 * O(1) removal without knowing whether the predecessor is the head or
 * another node: *pprev is "whatever pointer points at me".
 */
static inline void __hlist_del(struct hlist_node *n)
{
	struct hlist_node *next = n->next;
	struct hlist_node **pprev = n->pprev;

	*pprev = next;
	if (next)
		next->pprev = pprev;
}

static inline void hlist_del(struct hlist_node *n)
{
	__hlist_del(n);
	n->next = LIST_POISON1;
	n->pprev = LIST_POISON2;
}

static inline void hlist_del_init(struct hlist_node *n)
{
	if (!hlist_unhashed(n)) {
		__hlist_del(n);
		INIT_HLIST_NODE(n);
	}
}

#define hlist_entry(ptr, type, member) container_of(ptr, type, member)

#define hlist_entry_safe(ptr, type, member) ({				\
	typeof(ptr) ____ptr = (ptr);					\
	____ptr ? hlist_entry(____ptr, type, member) : NULL; })

#define hlist_for_each_entry(pos, head, member)				\
	for (pos = hlist_entry_safe((head)->first, typeof(*(pos)), member); \
	     pos;							\
	     pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member))

#define hlist_for_each_entry_safe(pos, n, head, member)		\
	for (pos = hlist_entry_safe((head)->first, typeof(*pos), member); \
	     pos && ({ n = pos->member.next; 1; });			\
	     pos = hlist_entry_safe(n, typeof(*pos), member))

#endif
