// SPDX-License-Identifier: MIT
/*
 * ostree.h - an order-statistic tree on the teaching red-black tree.
 *
 * Each node stores the number of nodes in its subtree.  That is enough to
 * find the k-th smallest key, or the rank of a node, in O(log n).
 * Equal keys are allowed; a new key goes after existing equal keys.
 */
#ifndef OSTREE_H
#define OSTREE_H

#include "rbtree.h"

struct os_node {
	struct rb_node rb;
	long key;
	unsigned long size;		/* nodes in this subtree */
};

void os_insert(struct os_node *node, struct rb_root *root);
void os_erase(struct os_node *node, struct rb_root *root);

struct os_node *os_select(const struct rb_root *root, unsigned long k);
unsigned long os_rank(const struct os_node *node);

#endif
