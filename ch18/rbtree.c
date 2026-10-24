// SPDX-License-Identifier: MIT
/*
 * rbtree.c - rebalancing for the teaching red-black tree (see rbtree.h).
 *
 * The algorithms are the ones the kernel uses: no sentinel nodes, an
 * insert fix-up that needs at most two rotations, an erase in two phases
 * (unlink, then fix up) that needs at most three, and an augmentation
 * hook called after every rotation.  The kernel writes the left and right
 * cases out separately and merges color and parent updates into single
 * stores; this version uses a direction index to keep the code short.
 */
#include "rbtree.h"

#define LEFT	0
#define RIGHT	1

typedef void (*rb_rotate_fn)(struct rb_node *old, struct rb_node *new);

static inline struct rb_node **rb_child(struct rb_node *n, int dir)
{
	return dir == LEFT ? &n->rb_left : &n->rb_right;
}

/* Make new take old's place below parent (or at the root). */
static inline void change_child(struct rb_node *old, struct rb_node *new,
				struct rb_node *parent, struct rb_root *root)
{
	if (!parent)
		root->rb_node = new;
	else if (parent->rb_left == old)
		parent->rb_left = new;
	else
		parent->rb_right = new;
}

/*
 * Rotate the subtree rooted at x so that x moves down on side dir and its
 * child y on the other side moves up.  Colors are left to the caller.
 */
static inline void rotate(struct rb_node *x, int dir, struct rb_root *root,
			  rb_rotate_fn augment_rotate)
{
	struct rb_node *y = *rb_child(x, !dir);
	struct rb_node *mid = *rb_child(y, dir);
	struct rb_node *parent = rb_parent(x);

	*rb_child(x, !dir) = mid;		/* y's inner subtree moves to x */
	if (mid)
		rb_set_parent(mid, x);
	*rb_child(y, dir) = x;
	rb_set_parent(y, parent);
	change_child(x, y, parent, root);
	rb_set_parent(x, y);
	augment_rotate(x, y);			/* y now roots x's old subtree */
}

/* Restore the red-black rules after a red node was linked in. */
static inline void insert_fixup(struct rb_node *node, struct rb_root *root,
				rb_rotate_fn augment_rotate)
{
	for (;;) {
		struct rb_node *parent = rb_parent(node), *gparent, *uncle;
		int dir;

		if (!parent) {				/* node is the root */
			rb_set_color(node, RB_BLACK);
			return;
		}
		if (rb_is_black(parent))		/* no red-red edge */
			return;
		gparent = rb_parent(parent);		/* a red node is never the root */
		dir = parent == gparent->rb_left ? LEFT : RIGHT;
		uncle = *rb_child(gparent, !dir);

		if (uncle && rb_is_red(uncle)) {
			/* Case 1: recolor and move the problem up two levels. */
			rb_set_color(parent, RB_BLACK);
			rb_set_color(uncle, RB_BLACK);
			rb_set_color(gparent, RB_RED);
			node = gparent;
			continue;
		}
		if (node == *rb_child(parent, !dir)) {
			/* Case 2: node is an inner grandchild; turn it outward. */
			rotate(parent, dir, root, augment_rotate);
			parent = node;
		}
		/* Case 3: one rotation at the grandparent finishes the job. */
		rotate(gparent, !dir, root, augment_rotate);
		rb_set_color(parent, RB_BLACK);
		rb_set_color(gparent, RB_RED);
		return;
	}
}

/*
 * Phase 1 of erase: unlink node.  Returns the node below which one black
 * node is now missing, or NULL if the tree is already valid.
 */
static inline struct rb_node *
erase_unlink(struct rb_node *node, struct rb_root *root,
	     const struct rb_augment_callbacks *aug)
{
	struct rb_node *left = node->rb_left, *right = node->rb_right;
	struct rb_node *parent = rb_parent(node);
	struct rb_node *rebalance = NULL, *fix_from;

	if (!left || !right) {
		/* At most one child: splice it into node's place. */
		struct rb_node *child = left ? left : right;

		change_child(node, child, parent, root);
		if (child)		/* a lone child is red: paint it black */
			rb_set_parent_color(child, parent, RB_BLACK);
		else if (rb_is_black(node))
			rebalance = parent;
		fix_from = parent;
	} else {
		/* Two children: the successor s takes node's place. */
		struct rb_node *s = right, *s_parent, *s_child;

		while (s->rb_left)
			s = s->rb_left;
		s_child = s->rb_right;
		if (s == right) {
			s_parent = s;		/* s keeps its right subtree */
			aug->copy(node, s);
		} else {
			s_parent = rb_parent(s);
			s_parent->rb_left = s_child;	/* unhook s ... */
			s->rb_right = right;		/* ... and give it node's right */
			rb_set_parent(right, s);
			aug->copy(node, s);
			aug->propagate(s_parent, s);	/* s_parent lost a node */
		}
		s->rb_left = left;
		rb_set_parent(left, s);
		change_child(node, s, parent, root);
		if (s_child)
			rb_set_parent_color(s_child, s_parent, RB_BLACK);
		else if (rb_is_black(s))
			rebalance = s_parent;
		s->__rb_parent_color = node->__rb_parent_color;
		fix_from = s;
	}
	aug->propagate(fix_from, NULL);
	return rebalance;
}

/*
 * Phase 2 of erase.  Every path through node, a child of parent, has one
 * black node too few.  node may be NULL: there is no sentinel, which is
 * why the loop tracks parent rather than node.
 */
static inline void erase_fixup(struct rb_node *parent, struct rb_root *root,
			       rb_rotate_fn augment_rotate)
{
	struct rb_node *node = NULL, *sibling, *near, *far;

	for (;;) {
		int dir = node == parent->rb_left ? LEFT : RIGHT;

		sibling = *rb_child(parent, !dir);	/* never NULL */
		if (rb_is_red(sibling)) {
			/* Case 1: rotate the red sibling up; recheck. */
			rotate(parent, dir, root, augment_rotate);
			rb_set_color(sibling, RB_BLACK);
			rb_set_color(parent, RB_RED);
			sibling = *rb_child(parent, !dir);
		}
		near = *rb_child(sibling, dir);
		far = *rb_child(sibling, !dir);
		if ((!near || rb_is_black(near)) && (!far || rb_is_black(far))) {
			/* Case 2: the sibling can turn red; move the deficit up. */
			rb_set_color(sibling, RB_RED);
			if (rb_is_red(parent)) {
				rb_set_color(parent, RB_BLACK);
				return;
			}
			node = parent;
			parent = rb_parent(node);
			if (!parent)
				return;
			continue;
		}
		if (!far || rb_is_black(far)) {
			/* Case 3: the near nephew is red; rotate it outward. */
			rotate(sibling, !dir, root, augment_rotate);
			rb_set_color(near, RB_BLACK);
			rb_set_color(sibling, RB_RED);
			far = sibling;
			sibling = near;
		}
		/* Case 4: the far nephew is red; one rotation ends it. */
		rotate(parent, dir, root, augment_rotate);
		rb_set_color(sibling, rb_color(parent));
		rb_set_color(parent, RB_BLACK);
		rb_set_color(far, RB_BLACK);
		return;
	}
}

/* Plain trees use empty callbacks, which the compiler removes. */
static void dummy_propagate(struct rb_node *n, struct rb_node *stop)
{
	(void)n, (void)stop;
}

static void dummy_copy(struct rb_node *old, struct rb_node *new)
{
	(void)old, (void)new;
}

static void dummy_rotate(struct rb_node *old, struct rb_node *new)
{
	(void)old, (void)new;
}

static const struct rb_augment_callbacks dummy_callbacks = {
	dummy_propagate, dummy_copy, dummy_rotate
};

void rb_insert_color(struct rb_node *node, struct rb_root *root)
{
	insert_fixup(node, root, dummy_rotate);
}

void rb_erase(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *rebalance = erase_unlink(node, root, &dummy_callbacks);

	if (rebalance)
		erase_fixup(rebalance, root, dummy_rotate);
}

void rb_insert_augmented(struct rb_node *node, struct rb_root *root,
			 const struct rb_augment_callbacks *aug)
{
	insert_fixup(node, root, aug->rotate);
}

void rb_erase_augmented(struct rb_node *node, struct rb_root *root,
			const struct rb_augment_callbacks *aug)
{
	struct rb_node *rebalance = erase_unlink(node, root, aug);

	if (rebalance)
		erase_fixup(rebalance, root, aug->rotate);
}

struct rb_node *rb_first(const struct rb_root *root)
{
	struct rb_node *n = root->rb_node;

	if (n)
		while (n->rb_left)
			n = n->rb_left;
	return n;
}

struct rb_node *rb_last(const struct rb_root *root)
{
	struct rb_node *n = root->rb_node;

	if (n)
		while (n->rb_right)
			n = n->rb_right;
	return n;
}

/* In-order successor: down-right-then-left, or up until we come from a left child. */
struct rb_node *rb_next(const struct rb_node *node)
{
	struct rb_node *parent;

	if (node->rb_right) {
		node = node->rb_right;
		while (node->rb_left)
			node = node->rb_left;
		return (struct rb_node *)node;
	}
	while ((parent = rb_parent(node)) && node == parent->rb_right)
		node = parent;
	return parent;
}

struct rb_node *rb_prev(const struct rb_node *node)
{
	struct rb_node *parent;

	if (node->rb_left) {
		node = node->rb_left;
		while (node->rb_right)
			node = node->rb_right;
		return (struct rb_node *)node;
	}
	while ((parent = rb_parent(node)) && node == parent->rb_left)
		node = parent;
	return parent;
}
