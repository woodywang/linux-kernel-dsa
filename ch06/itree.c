// SPDX-License-Identifier: MIT
/*
 * itree.c - an interval tree built with RB_DECLARE_CALLBACKS (see itree.h).
 * The kernel generates the same functions for any node type with the
 * INTERVAL_TREE_DEFINE() template in include/linux/interval_tree_generic.h.
 */
#include "itree.h"

#define it_entry(p)	rb_entry(p, struct it_node, rb)

/* subtree_last = max(last, children's subtree_last) */
static inline bool it_compute(struct it_node *n, bool exit)
{
	unsigned long max = n->last;

	if (n->rb.rb_left && it_entry(n->rb.rb_left)->subtree_last > max)
		max = it_entry(n->rb.rb_left)->subtree_last;
	if (n->rb.rb_right && it_entry(n->rb.rb_right)->subtree_last > max)
		max = it_entry(n->rb.rb_right)->subtree_last;
	if (exit && n->subtree_last == max)
		return true;
	n->subtree_last = max;
	return false;
}

RB_DECLARE_CALLBACKS(static, it_augment, struct it_node, rb,
		     subtree_last, it_compute)

void it_insert(struct it_node *node, struct rb_root_cached *root)
{
	struct rb_node **link = &root->rb_root.rb_node, *parent = NULL;
	bool leftmost = true;

	while (*link) {
		struct it_node *p = it_entry(*link);

		parent = *link;
		if (p->subtree_last < node->last)	/* node will be below p */
			p->subtree_last = node->last;
		if (node->start < p->start) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = false;
		}
	}
	node->subtree_last = node->last;
	rb_link_node(&node->rb, parent, link);
	rb_insert_augmented_cached(&node->rb, root, leftmost, &it_augment);
}

void it_remove(struct it_node *node, struct rb_root_cached *root)
{
	rb_erase_augmented_cached(&node->rb, root, &it_augment);
}

/*
 * Find the leftmost interval below n that overlaps [start, last].
 * Precondition: n->subtree_last >= start.
 */
static struct it_node *subtree_search(struct it_node *n, unsigned long start,
				      unsigned long last)
{
	for (;;) {
		struct rb_node *l = n->rb.rb_left;

		if (l && it_entry(l)->subtree_last >= start) {
			n = it_entry(l);	/* something on the left reaches start */
			continue;
		}
		if (n->start > last)
			return NULL;		/* n and all to its right start too late */
		if (n->last >= start)
			return n;		/* overlaps */
		n = it_entry(n->rb.rb_right);	/* must exist, by the precondition */
	}
}

struct it_node *it_iter_first(struct rb_root_cached *root,
			      unsigned long start, unsigned long last)
{
	struct it_node *top;

	if (!root->rb_root.rb_node)
		return NULL;
	top = it_entry(root->rb_root.rb_node);
	if (top->subtree_last < start)		/* everything ends too early */
		return NULL;
	if (it_entry(root->rb_leftmost)->start > last)	/* or starts too late */
		return NULL;
	return subtree_search(top, start, last);
}

struct it_node *it_iter_next(struct it_node *n, unsigned long start,
			     unsigned long last)
{
	struct rb_node *rb = n->rb.rb_right, *prev;

	for (;;) {
		/* Later intervals are in n's right subtree or above it. */
		if (rb && it_entry(rb)->subtree_last >= start)
			return subtree_search(it_entry(rb), start, last);
		do {	/* climb until we arrive from a left child */
			rb = rb_parent(&n->rb);
			if (!rb)
				return NULL;
			prev = &n->rb;
			n = it_entry(rb);
			rb = n->rb.rb_right;
		} while (prev == rb);
		if (n->start > last)
			return NULL;
		if (n->last >= start)
			return n;
	}
}
