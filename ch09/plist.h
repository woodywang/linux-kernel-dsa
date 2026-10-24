// SPDX-License-Identifier: MIT
/*
 * plist.h - a teaching version of the kernel's priority-sorted list
 * (include/linux/plist.h, lib/plist.c).
 *
 * A plist is a list of lists.  Every node is on node_list, which holds all
 * nodes sorted by priority (lower value = higher priority), FIFO among
 * equal priorities.  The first node of each priority is also on prio_list,
 * a circular list with no head that holds exactly one node per distinct
 * priority.  Adding walks prio_list, so it costs O(K) for K distinct
 * priorities however many nodes there are; removing and finding the first
 * node cost O(1).
 *
 * What it leaves out on purpose: since 6.11 the kernel's plist_add() walks
 * prio_list from both ends at once (Exercise 9.6); its plist_requeue() has
 * a shortcut this version does not; and the CONFIG_DEBUG_PLIST checks are
 * replaced by plist_check() for the tests.
 */
#ifndef PLIST_H
#define PLIST_H

#include <stdbool.h>
#include "list.h"

struct plist_head {
	struct list_head node_list;
};

struct plist_node {
	int prio;
	struct list_head prio_list;	/* on it: first of its priority */
	struct list_head node_list;	/* every node */
};

static inline void plist_head_init(struct plist_head *head)
{
	INIT_LIST_HEAD(&head->node_list);
}

static inline void plist_node_init(struct plist_node *node, int prio)
{
	node->prio = prio;
	INIT_LIST_HEAD(&node->prio_list);
	INIT_LIST_HEAD(&node->node_list);
}

static inline bool plist_head_empty(const struct plist_head *head)
{
	return list_empty(&head->node_list);
}

static inline bool plist_node_empty(const struct plist_node *node)
{
	return list_empty(&node->node_list);
}

/* The highest-priority node (smallest prio), O(1).  Head must be non-empty. */
static inline struct plist_node *plist_first(const struct plist_head *head)
{
	return list_entry(head->node_list.next, struct plist_node, node_list);
}

static inline struct plist_node *plist_last(const struct plist_head *head)
{
	return list_entry(head->node_list.prev, struct plist_node, node_list);
}

#define plist_for_each_entry(pos, head, mem)				\
	list_for_each_entry(pos, &(head)->node_list, mem.node_list)

void plist_add(struct plist_node *node, struct plist_head *head);
void plist_del(struct plist_node *node, struct plist_head *head);
void plist_requeue(struct plist_node *node, struct plist_head *head);
bool plist_check(const struct plist_head *head);	/* for the tests */

#endif /* PLIST_H */
