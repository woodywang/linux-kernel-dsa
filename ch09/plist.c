// SPDX-License-Identifier: MIT
/* plist.c - add, delete and requeue for plist.h.  See plist.h. */
#include "plist.h"

static struct plist_node *next_prio(struct plist_node *n)
{
	return list_entry(n->prio_list.next, struct plist_node, prio_list);
}

void plist_add(struct plist_node *node, struct plist_head *head)
{
	struct list_head *before = &head->node_list;	/* default: append */

	if (!plist_head_empty(head)) {
		struct plist_node *first = plist_first(head), *iter = first;
		struct plist_node *prev = NULL;

		/* Find the first priority group that sorts after node. */
		do {
			if (node->prio < iter->prio) {
				before = &iter->node_list;
				break;
			}
			prev = iter;
			iter = next_prio(iter);
		} while (iter != first);

		/* A priority not seen before joins prio_list, before iter. */
		if (!prev || prev->prio != node->prio)
			list_add_tail(&node->prio_list, &iter->prio_list);
	}
	/* On node_list, node goes last among its equals: FIFO. */
	list_add_tail(&node->node_list, before);
}

void plist_del(struct plist_node *node, struct plist_head *head)
{
	if (!list_empty(&node->prio_list)) {
		/* node stood for its priority; its successor may take over. */
		if (node->node_list.next != &head->node_list) {
			struct plist_node *next = list_entry(node->node_list.next,
						struct plist_node, node_list);

			if (list_empty(&next->prio_list))	/* same prio */
				list_add(&next->prio_list, &node->prio_list);
		}
		list_del_init(&node->prio_list);
	}
	list_del_init(&node->node_list);
}

/* Move node behind the other nodes of its priority (round robin). */
void plist_requeue(struct plist_node *node, struct plist_head *head)
{
	plist_del(node, head);
	plist_add(node, head);
}

/*
 * The invariants: node_list is sorted; prio_list links exactly the first
 * node of each priority, in increasing order.  With a single priority that
 * circle is one node linked to itself, which looks like an empty list.
 */
bool plist_check(const struct plist_head *head)
{
	const struct plist_node *n, *rep = NULL;
	int last = 0, groups = 0;

	list_for_each_entry(n, &head->node_list, node_list)
		if (!groups || n->prio != last) {
			groups++;
			last = n->prio;
		}
	list_for_each_entry(n, &head->node_list, node_list) {
		bool first_of_prio = !rep || n->prio != last;

		if (rep && n->prio < last)
			return false;			/* out of order */
		if ((first_of_prio && groups > 1) !=
		    !list_empty(&n->prio_list))
			return false;			/* wrong rep set */
		if (first_of_prio) {
			if (rep && n->prio_list.prev != &rep->prio_list)
				return false;		/* prio_list order */
			rep = n;
		}
		last = n->prio;
	}
	/* The circle closes: the last rep links back to the first. */
	return groups < 2 || rep->prio_list.next ==
		&plist_first(head)->prio_list;
}
