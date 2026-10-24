// SPDX-License-Identifier: MIT
/*
 * llist.h - a lock-free, NULL-terminated singly linked list in the style
 * of the kernel's llist (include/linux/llist.h, lib/llist.c).
 *
 * The list is a stack: producers push at the head with a compare-and-swap
 * loop, and consumers either take the whole list at once with one atomic
 * exchange (llist_del_all) or pop the newest node (llist_del_first).
 * The rules, as in the kernel header:
 *
 *   - llist_add() needs no lock against anything;
 *   - llist_del_all() needs no lock against llist_add() or itself;
 *   - llist_del_first() is safe against llist_add() only if there is a
 *     single caller of llist_del_first() / llist_del_all() at a time;
 *     otherwise the consumers need a lock (see aba_demo.c for why).
 *
 * Nodes taken off the list come newest first; llist_reverse_order()
 * turns a detached chain around.
 *
 * Ordering: a producer fills in a node and links it with a release CAS;
 * consumers read the head with acquire, so they see the node's contents
 * (the kernel's cmpxchg() and xchg() are fully ordered, which is more).
 *
 * Deliberately left out: the non-atomic __llist_* variants, the "on list"
 * marker (llist_on_list), llist_del_first_this(), and the NMI rules.
 */
#ifndef LLIST_H
#define LLIST_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

struct llist_node {
	struct llist_node *next;
};

struct llist_head {
	struct llist_node *_Atomic first;
};

static inline void init_llist_head(struct llist_head *head)
{
	atomic_init(&head->first, NULL);
}

/* Push new_first..new_last (already linked); true if the list was empty. */
static inline bool llist_add_batch(struct llist_node *new_first,
				   struct llist_node *new_last,
				   struct llist_head *head)
{
	struct llist_node *first =
		atomic_load_explicit(&head->first, memory_order_relaxed);

	do {
		new_last->next = first;
	} while (!atomic_compare_exchange_weak_explicit(&head->first, &first,
			new_first, memory_order_release, memory_order_relaxed));
	return first == NULL;
}

static inline bool llist_add(struct llist_node *node, struct llist_head *head)
{
	return llist_add_batch(node, node, head);
}

/* Take every node at once; returns the newest, or NULL. */
static inline struct llist_node *llist_del_all(struct llist_head *head)
{
	return atomic_exchange_explicit(&head->first, NULL,
					memory_order_acquire);
}

/* Pop the newest node.  Only ONE consumer may do this at a time. */
static inline struct llist_node *llist_del_first(struct llist_head *head)
{
	struct llist_node *entry, *next;

	entry = atomic_load_explicit(&head->first, memory_order_acquire);
	do {
		if (entry == NULL)
			return NULL;
		next = entry->next;
	} while (!atomic_compare_exchange_weak_explicit(&head->first, &entry,
			next, memory_order_acquire, memory_order_acquire));
	return entry;
}

/* Reverse a detached chain, so that it runs oldest first. */
static inline struct llist_node *llist_reverse_order(struct llist_node *head)
{
	struct llist_node *new_head = NULL;

	while (head) {
		struct llist_node *tmp = head;

		head = head->next;
		tmp->next = new_head;
		new_head = tmp;
	}
	return new_head;
}

static inline bool llist_empty(struct llist_head *head)
{
	return atomic_load_explicit(&head->first, memory_order_relaxed) == NULL;
}

#define llist_for_each(pos, node) \
	for ((pos) = (node); (pos); (pos) = (pos)->next)

#endif /* LLIST_H */
