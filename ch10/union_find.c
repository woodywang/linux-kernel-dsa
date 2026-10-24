// SPDX-License-Identifier: MIT
/*
 * union_find.c - find with path splitting, union by rank.
 *
 * The loop in uf_find() makes every node on the path point to its
 * grandparent as it walks up: one pass, no recursion, no second loop.
 * (Full path compression, which points every node at the root, needs
 * either recursion or a second pass.)  Both give the same amortized bound.
 */
#include "union_find.h"

struct uf_node *uf_find(struct uf_node *node)
{
	struct uf_node *parent;

	while (node->parent != node) {
		parent = node->parent;
		node->parent = parent->parent;	/* skip a level */
		node = parent;
	}
	return node;
}

void uf_union(struct uf_node *node1, struct uf_node *node2)
{
	struct uf_node *root1 = uf_find(node1);
	struct uf_node *root2 = uf_find(node2);

	if (root1 == root2)
		return;
	if (root1->rank < root2->rank) {
		root1->parent = root2;		/* shorter tree goes below */
	} else if (root1->rank > root2->rank) {
		root2->parent = root1;
	} else {
		root2->parent = root1;
		root1->rank++;
	}
}
