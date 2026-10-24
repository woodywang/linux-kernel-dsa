// SPDX-License-Identifier: MIT
/*
 * itree.h - an interval tree on the teaching red-black tree.
 *
 * Nodes are sorted by start; each node also stores the largest "last"
 * in its subtree, which lets a query skip subtrees that end too early.
 * Intervals are closed, [start, last], as in the kernel's interval trees.
 */
#ifndef ITREE_H
#define ITREE_H

#include "rbtree.h"

struct it_node {
	struct rb_node rb;
	unsigned long start, last;	/* the interval [start, last] */
	unsigned long subtree_last;	/* max of last over this subtree */
};

void it_insert(struct it_node *node, struct rb_root_cached *root);
void it_remove(struct it_node *node, struct rb_root_cached *root);

/* Visit every interval that overlaps [start, last], in order of start. */
struct it_node *it_iter_first(struct rb_root_cached *root,
			      unsigned long start, unsigned long last);
struct it_node *it_iter_next(struct it_node *node,
			     unsigned long start, unsigned long last);

#endif
