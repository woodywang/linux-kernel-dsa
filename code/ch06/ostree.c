// SPDX-License-Identifier: MIT
/*
 * ostree.c - subtree sizes as an augmentation (see ostree.h).  The kernel
 * has no order-statistic tree; this one shows that the same callbacks that
 * maintain a maximum can maintain a count.
 */
#include "ostree.h"

#define os_entry(p)	rb_entry(p, struct os_node, rb)

static inline unsigned long os_size(const struct rb_node *rb)
{
	return rb ? os_entry(rb)->size : 0;
}

/* size = 1 + size(left) + size(right) */
static inline bool os_compute(struct os_node *n, bool exit)
{
	unsigned long size = 1 + os_size(n->rb.rb_left) + os_size(n->rb.rb_right);

	if (exit && n->size == size)
		return true;
	n->size = size;
	return false;
}

RB_DECLARE_CALLBACKS(static, os_augment, struct os_node, rb, size, os_compute)

void os_insert(struct os_node *node, struct rb_root *root)
{
	struct rb_node **link = &root->rb_node, *parent = NULL;

	while (*link) {
		parent = *link;
		os_entry(parent)->size++;	/* node will be below parent */
		if (node->key < os_entry(parent)->key)
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}
	node->size = 1;
	rb_link_node(&node->rb, parent, link);
	rb_insert_augmented(&node->rb, root, &os_augment);
}

void os_erase(struct os_node *node, struct rb_root *root)
{
	rb_erase_augmented(&node->rb, root, &os_augment);
}

/* The k-th smallest node, counting from 0, or NULL if k >= size. */
struct os_node *os_select(const struct rb_root *root, unsigned long k)
{
	struct rb_node *rb = root->rb_node;

	while (rb) {
		unsigned long left = os_size(rb->rb_left);

		if (k < left) {
			rb = rb->rb_left;
		} else if (k == left) {
			return os_entry(rb);
		} else {
			k -= left + 1;		/* skip the left subtree and rb */
			rb = rb->rb_right;
		}
	}
	return NULL;
}

/* How many nodes come before node in order. */
unsigned long os_rank(const struct os_node *node)
{
	const struct rb_node *rb = &node->rb, *parent;
	unsigned long rank = os_size(rb->rb_left);

	while ((parent = rb_parent(rb))) {
		if (rb == parent->rb_right)	/* parent and its left come first */
			rank += os_size(parent->rb_left) + 1;
		rb = parent;
	}
	return rank;
}
