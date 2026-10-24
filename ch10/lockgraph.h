// SPDX-License-Identifier: MIT
/*
 * lockgraph.h - a lock-dependency graph with lockdep-style cycle checks.
 * See lockgraph.c.
 */
#ifndef LOCKGRAPH_H
#define LOCKGRAPH_H

#include <stdbool.h>
#include "ilist.h"

#define CQ_MAX_BITS	12		/* queue of 4096 slots, as lockdep's default */
#define MAX_DEPS	16384		/* edges come from a fixed pool */

struct lock_class {
	const char *name;
	struct list_head locks_after;	/* edges to locks taken while holding this */
	unsigned int gen;		/* == graph's gen: expanded in this search */
};

/* One dependency edge "this lock was taken while holding the list's owner". */
struct dep {
	struct list_head entry;		/* on the owner's locks_after list */
	struct lock_class *class;	/* the lock this edge leads to */
	struct dep *parent;		/* BFS: the edge we arrived by */
};

struct circular_queue {
	struct dep *element[1 << CQ_MAX_BITS];
	unsigned int front, rear, mask;
};

struct lock_graph {
	struct circular_queue cq;
	unsigned int gen;		/* search generation */
	bool disabled;			/* gave up after running out of room */
	int nr_deps;
	struct dep pool[MAX_DEPS];
	int max_queue;			/* statistics */
};

enum bfs_result {
	BFS_EQUEUEFULL = -1,
	BFS_MATCH = 0,
	BFS_NOMATCH = 1,
};

void lock_graph_init(struct lock_graph *g, unsigned int cq_bits);
void lock_class_init(struct lock_class *c, const char *name);
int add_dependency(struct lock_graph *g, struct lock_class *prev,
		   struct lock_class *next, struct lock_class **path, int *len);
bool has_edge(struct lock_class *from, struct lock_class *to);

#endif
