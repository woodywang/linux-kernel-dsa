// SPDX-License-Identifier: MIT
/*
 * union_find.h - disjoint sets with union by rank and path splitting,
 * in the style of the kernel's lib/union_find.c.  The node is meant to be
 * embedded in the object it describes, like a list_head.
 */
#ifndef UNION_FIND_H
#define UNION_FIND_H

struct uf_node {
	struct uf_node *parent;		/* points to itself at a root */
	unsigned int rank;		/* upper bound on the height below */
};

static inline void uf_node_init(struct uf_node *node)
{
	node->parent = node;
	node->rank = 0;
}

struct uf_node *uf_find(struct uf_node *node);
void uf_union(struct uf_node *node1, struct uf_node *node2);

#endif
