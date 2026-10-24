// SPDX-License-Identifier: MIT
/*
 * lockgraph.c - lock-dependency cycle checks in the style of lockdep.
 *
 * Each lock class keeps an intrusive list of outgoing dependency edges.
 * Before an edge prev -> next is added, a breadth-first search from next
 * looks for prev; if it is reachable, the new edge would close a cycle (a
 * possible deadlock) and the shortest such cycle is reported.
 *
 * The search mirrors lockdep's design:
 *   - a fixed-size circular queue, allocated once; if it fills up, the
 *     search fails and the checker switches itself off rather than guess;
 *   - "visited" marks are a generation number in each class, so starting a
 *     new search clears every mark in O(1);
 *   - only the first edge of each list is queued; its siblings are reached
 *     through the parent pointer, which keeps the queue short;
 *   - parent pointers live in the edges themselves, so the path can be
 *     printed without extra memory.
 *
 * What it leaves out on purpose: read/write lock kinds and the "strong
 * path" rules, IRQ-context checks, backward edge lists, the cache of
 * already-validated lock chains, stack traces, and all locking (lockdep
 * runs its searches under one global graph lock).
 *
 * Path arrays passed to add_dependency() need room for every lock class.
 */
#include <errno.h>
#include "lockgraph.h"

void lock_graph_init(struct lock_graph *g, unsigned int cq_bits)
{
	g->cq.mask = (1u << cq_bits) - 1;
	g->gen = 0;
	g->disabled = false;
	g->nr_deps = 0;
	g->max_queue = 0;
}

void lock_class_init(struct lock_class *c, const char *name)
{
	c->name = name;
	list_init(&c->locks_after);
	c->gen = 0;
}

/* ---- the circular queue: power-of-two size, one slot kept empty -------- */

static void cq_init(struct lock_graph *g)
{
	g->cq.front = g->cq.rear = 0;
	g->gen++;			/* forget every mark of the last search */
}

static int cq_enqueue(struct circular_queue *cq, struct dep *d)
{
	if (((cq->rear + 1) & cq->mask) == cq->front)
		return -1;		/* full */
	cq->element[cq->rear] = d;
	cq->rear = (cq->rear + 1) & cq->mask;
	return 0;
}

static struct dep *cq_dequeue(struct circular_queue *cq)
{
	struct dep *d;

	if (cq->front == cq->rear)
		return NULL;		/* empty */
	d = cq->element[cq->front];
	cq->front = (cq->front + 1) & cq->mask;
	return d;
}

/* ---- breadth-first search --------------------------------------------- */

/* The next edge on the same list as d, found through d's parent. */
static struct dep *next_sibling(struct dep *d)
{
	struct list_head *head;

	if (!d || !d->parent)
		return NULL;
	head = &d->parent->class->locks_after;
	if (d->entry.next == head)
		return NULL;
	return list_entry(d->entry.next, struct dep, entry);
}

static enum bfs_result bfs(struct lock_graph *g, struct dep *src,
			   struct lock_class *target, struct dep **found)
{
	struct dep *d = NULL, *e;

	cq_init(g);
	cq_enqueue(&g->cq, src);

	while ((d = next_sibling(d)) || (d = cq_dequeue(&g->cq))) {
		if (d->class->gen == g->gen)
			continue;		/* this class was expanded already */
		d->class->gen = g->gen;

		if (d->class == target) {
			*found = d;
			return BFS_MATCH;
		}

		bool first = true;

		list_for_each_entry(e, &d->class->locks_after, entry) {
			e->parent = d;
			if (!first)
				continue;	/* reached later via next_sibling() */
			first = false;
			if (cq_enqueue(&g->cq, e))
				return BFS_EQUEUEFULL;
			int depth = (g->cq.rear - g->cq.front) & g->cq.mask;

			if (depth > g->max_queue)
				g->max_queue = depth;
		}
	}
	return BFS_NOMATCH;
}

bool has_edge(struct lock_class *from, struct lock_class *to)
{
	struct dep *e;

	list_for_each_entry(e, &from->locks_after, entry)
		if (e->class == to)
			return true;
	return false;
}

/*
 * Record that next was taken while prev was held.  Returns 0 if the edge
 * is fine (or the checker is off), -EDEADLK if it closes a cycle (path[]
 * then holds next ... prev, *len classes), -ENOSPC if the checker ran out
 * of room and switched itself off.
 */
int add_dependency(struct lock_graph *g, struct lock_class *prev,
		   struct lock_class *next, struct lock_class **path, int *len)
{
	struct dep root = { .class = next, .parent = NULL };
	struct dep *found, *d;
	int n = 0;

	*len = 0;
	if (g->disabled)
		return 0;

	switch (bfs(g, &root, prev, &found)) {	/* can next reach prev? */
	case BFS_MATCH:
		for (d = found; d; d = d->parent)
			n++;
		*len = n;
		for (d = found; d; d = d->parent)
			path[--n] = d->class;
		return -EDEADLK;
	case BFS_EQUEUEFULL:
		g->disabled = true;		/* give up, don't guess */
		return -ENOSPC;
	case BFS_NOMATCH:
		break;
	}

	if (has_edge(prev, next))
		return 0;
	if (g->nr_deps == MAX_DEPS) {
		g->disabled = true;
		return -ENOSPC;
	}
	d = &g->pool[g->nr_deps++];
	d->class = next;
	d->parent = NULL;
	list_add_tail(&d->entry, &prev->locks_after);
	return 0;
}
