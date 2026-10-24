// SPDX-License-Identifier: MIT
/*
 * rbtree.h - a small teaching version of the kernel's red-black tree.
 *
 * It follows the kernel's design:
 *   - the node is intrusive (embed a struct rb_node in your object) and is
 *     three words long: the parent pointer with the color packed into
 *     bit 0, and the two child pointers;
 *   - there is no comparison callback: the caller writes the search loop,
 *     links the new node with rb_link_node(), and calls rb_insert_color()
 *     to rebalance;
 *   - rb_root_cached remembers the leftmost node;
 *   - augmented trees keep per-subtree data up to date through three
 *     callbacks (propagate, copy, rotate), and RB_DECLARE_CALLBACKS writes
 *     them from one "compute" function.
 *
 * What it leaves out on purpose: the kernel's unrolled mirror-image cases
 * (we use a direction index instead), WRITE_ONCE() stores for lockless
 * lookups, the RCU variants, rb_replace_node(), postorder iteration, and
 * the rb_add()/rb_find() helpers.  Locking is the caller's job, as in the
 * kernel.
 */
#ifndef RBTREE_H
#define RBTREE_H

#include <stdbool.h>
#include <stddef.h>
#include "container_of.h"

struct rb_node {
	unsigned long __rb_parent_color;	/* parent | color (bit 0) */
	struct rb_node *rb_right;
	struct rb_node *rb_left;
} __attribute__((aligned(sizeof(long))));

struct rb_root {
	struct rb_node *rb_node;
};

/* A root that also remembers the leftmost (smallest) node. */
struct rb_root_cached {
	struct rb_root rb_root;
	struct rb_node *rb_leftmost;
};

#define RB_ROOT		(struct rb_root) { NULL }
#define RB_ROOT_CACHED	(struct rb_root_cached) { { NULL }, NULL }

#define RB_RED		0
#define RB_BLACK	1

/* Nodes are word-aligned, so the low bits of the parent pointer are free. */
#define rb_parent(n)	((struct rb_node *)((n)->__rb_parent_color & ~3UL))
#define rb_color(n)	((int)((n)->__rb_parent_color & 1))
#define rb_is_red(n)	(rb_color(n) == RB_RED)
#define rb_is_black(n)	(rb_color(n) == RB_BLACK)

#define rb_entry(ptr, type, member) container_of(ptr, type, member)

/* A node that is in no tree points to itself. */
#define RB_EMPTY_NODE(n)  ((n)->__rb_parent_color == (unsigned long)(n))
#define RB_CLEAR_NODE(n)  ((n)->__rb_parent_color = (unsigned long)(n))

static inline void rb_set_parent_color(struct rb_node *n,
				       struct rb_node *parent, int color)
{
	n->__rb_parent_color = (unsigned long)parent | color;
}

static inline void rb_set_parent(struct rb_node *n, struct rb_node *parent)
{
	rb_set_parent_color(n, parent, rb_color(n));
}

static inline void rb_set_color(struct rb_node *n, int color)
{
	rb_set_parent_color(n, rb_parent(n), color);
}

/*
 * Hang a new (red) node at *link, below parent.  The caller found link
 * with its own search loop; rb_insert_color() then restores the balance.
 */
static inline void rb_link_node(struct rb_node *node, struct rb_node *parent,
				struct rb_node **link)
{
	rb_set_parent_color(node, parent, RB_RED);
	node->rb_left = node->rb_right = NULL;
	*link = node;
}

void rb_insert_color(struct rb_node *node, struct rb_root *root);
void rb_erase(struct rb_node *node, struct rb_root *root);

struct rb_node *rb_first(const struct rb_root *root);
struct rb_node *rb_last(const struct rb_root *root);
struct rb_node *rb_next(const struct rb_node *node);
struct rb_node *rb_prev(const struct rb_node *node);

/* Cached roots: O(1) access to the smallest node. */
#define rb_first_cached(root)	((root)->rb_leftmost)

static inline void rb_insert_color_cached(struct rb_node *node,
					  struct rb_root_cached *root,
					  bool leftmost)
{
	if (leftmost)
		root->rb_leftmost = node;
	rb_insert_color(node, &root->rb_root);
}

/* Returns the new leftmost node if node was the leftmost, else NULL. */
static inline struct rb_node *rb_erase_cached(struct rb_node *node,
					      struct rb_root_cached *root)
{
	struct rb_node *leftmost = NULL;

	if (root->rb_leftmost == node)
		leftmost = root->rb_leftmost = rb_next(node);
	rb_erase(node, &root->rb_root);
	return leftmost;
}

/*
 * Augmented trees.  Each node keeps a value computed from its subtree.
 *   propagate(n, stop): recompute n and its ancestors, up to (not
 *                       including) stop; may end early when a value
 *                       does not change.
 *   copy(old, new):     new now roots the subtree old rooted; copy the value.
 *   rotate(old, new):   a rotation made new the root of old's subtree:
 *                       copy old's value to new, recompute old.
 */
struct rb_augment_callbacks {
	void (*propagate)(struct rb_node *node, struct rb_node *stop);
	void (*copy)(struct rb_node *old, struct rb_node *new);
	void (*rotate)(struct rb_node *old, struct rb_node *new);
};

void rb_insert_augmented(struct rb_node *node, struct rb_root *root,
			 const struct rb_augment_callbacks *aug);
void rb_erase_augmented(struct rb_node *node, struct rb_root *root,
			const struct rb_augment_callbacks *aug);

static inline void
rb_insert_augmented_cached(struct rb_node *node, struct rb_root_cached *root,
			   bool leftmost, const struct rb_augment_callbacks *aug)
{
	if (leftmost)
		root->rb_leftmost = node;
	rb_insert_augmented(node, &root->rb_root, aug);
}

static inline void
rb_erase_augmented_cached(struct rb_node *node, struct rb_root_cached *root,
			  const struct rb_augment_callbacks *aug)
{
	if (root->rb_leftmost == node)
		root->rb_leftmost = rb_next(node);
	rb_erase_augmented(node, &root->rb_root, aug);
}

/*
 * RB_DECLARE_CALLBACKS(RBSTATIC, RBNAME, RBSTRUCT, RBFIELD, RBAUGMENTED,
 *                      RBCOMPUTE)
 * Write the three callbacks from one function:
 *   bool RBCOMPUTE(RBSTRUCT *node, bool exit)
 * recomputes node->RBAUGMENTED from the node and its two children.  If
 * exit is true and the value did not change, it returns true without
 * storing, so that propagation can stop there.
 */
#define RB_DECLARE_CALLBACKS(RBSTATIC, RBNAME, RBSTRUCT, RBFIELD,	\
			     RBAUGMENTED, RBCOMPUTE)			\
static void RBNAME ## _propagate(struct rb_node *rb, struct rb_node *stop) \
{									\
	while (rb != stop) {						\
		RBSTRUCT *n = rb_entry(rb, RBSTRUCT, RBFIELD);		\
		if (RBCOMPUTE(n, true))					\
			break;	/* unchanged: ancestors are too */	\
		rb = rb_parent(&n->RBFIELD);				\
	}								\
}									\
static void RBNAME ## _copy(struct rb_node *rb_old, struct rb_node *rb_new) \
{									\
	RBSTRUCT *old = rb_entry(rb_old, RBSTRUCT, RBFIELD);		\
	RBSTRUCT *new = rb_entry(rb_new, RBSTRUCT, RBFIELD);		\
	new->RBAUGMENTED = old->RBAUGMENTED;				\
}									\
static void RBNAME ## _rotate(struct rb_node *rb_old, struct rb_node *rb_new) \
{									\
	RBSTRUCT *old = rb_entry(rb_old, RBSTRUCT, RBFIELD);		\
	RBSTRUCT *new = rb_entry(rb_new, RBSTRUCT, RBFIELD);		\
	new->RBAUGMENTED = old->RBAUGMENTED;	/* same set of nodes */	\
	RBCOMPUTE(old, false);			/* old lost a subtree */ \
}									\
RBSTATIC const struct rb_augment_callbacks RBNAME = {			\
	.propagate = RBNAME ## _propagate,				\
	.copy = RBNAME ## _copy,					\
	.rotate = RBNAME ## _rotate,					\
};

#endif
