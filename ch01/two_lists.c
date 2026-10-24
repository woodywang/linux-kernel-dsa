// SPDX-License-Identifier: MIT
/*
 * two_lists.c - the same job done with an intrusive list and with a
 * "container" list that allocates a node per membership.
 *
 * Every task is on the list of all tasks and on a run queue.  We then take
 * every third task off the run queue, the way a scheduler would when a task
 * goes to sleep, and count what each design had to do.
 *
 *   cc -O2 -Wall -o two_lists two_lists.c && ./two_lists
 */
#include <stdio.h>
#include <stdlib.h>
#include "ilist.h"

#define N 10000

static long allocs, link_bytes, steps;

static void *xmalloc(size_t n)
{
	void *p = malloc(n);

	if (!p)
		abort();
	allocs++;
	return p;
}

/* ---- Intrusive: the links live inside the task ------------------------- */

struct task {
	int pid;
	struct list_head all;	/* on the list of all tasks */
	struct list_head run;	/* on the run queue */
};

static void intrusive(void)
{
	struct list_head all = LIST_HEAD_INIT(all), runq = LIST_HEAD_INIT(runq);
	struct task *t, *tmp;
	int i;

	allocs = link_bytes = steps = 0;
	for (i = 0; i < N; i++) {
		t = xmalloc(sizeof(*t));
		t->pid = i;
		list_add_tail(&t->all, &all);
		list_add_tail(&t->run, &runq);
	}
	link_bytes = 0;			/* links are part of struct task */

	/* Every third task goes to sleep.  We already hold the task, so
	 * removing it from the run queue is O(1): no search. */
	list_for_each_entry(t, &all, all) {
		if (t->pid % 3 == 0) {
			list_del(&t->run);
			steps++;
		}
	}
	printf("intrusive: %6ld allocations, %7ld bytes of separate link nodes, "
	       "%9ld steps to dequeue\n", allocs, link_bytes, steps);

	list_for_each_entry_safe(t, tmp, &all, all)
		free(t);
}

/* ---- Container: the list owns nodes that point at the task ------------ */

struct cnode {
	struct cnode *next, *prev;
	void *obj;
};

struct ctask {
	int pid;		/* the task knows nothing about lists */
};

static void cl_init(struct cnode *h)
{
	h->next = h->prev = h;
}

static void cl_add_tail(struct cnode *h, void *obj)
{
	struct cnode *n = xmalloc(sizeof(*n));

	link_bytes += sizeof(*n);
	n->obj = obj;
	n->prev = h->prev;
	n->next = h;
	h->prev->next = n;
	h->prev = n;
}

/* Given only the object, the list must be searched to find its node. */
static void cl_remove(struct cnode *h, void *obj)
{
	struct cnode *n;

	for (n = h->next; n != h; n = n->next) {
		steps++;
		if (n->obj == obj) {
			n->prev->next = n->next;
			n->next->prev = n->prev;
			free(n);
			return;
		}
	}
}

static void container(void)
{
	struct cnode all, runq, *n, *next;
	int i;

	allocs = link_bytes = steps = 0;
	cl_init(&all);
	cl_init(&runq);
	for (i = 0; i < N; i++) {
		struct ctask *t = xmalloc(sizeof(*t));

		t->pid = i;
		cl_add_tail(&all, t);
		cl_add_tail(&runq, t);
	}
	for (n = all.next; n != &all; n = n->next) {
		struct ctask *t = n->obj;

		if (t->pid % 3 == 0)
			cl_remove(&runq, t);
	}
	printf("container: %6ld allocations, %7ld bytes of separate link nodes, "
	       "%9ld steps to dequeue\n", allocs, link_bytes, steps);

	for (n = runq.next; n != &runq; n = next) {
		next = n->next;
		free(n);
	}
	for (n = all.next; n != &all; n = next) {
		next = n->next;
		free(n->obj);
		free(n);
	}
}

int main(void)
{
	printf("%d tasks, each on two lists; every third task is dequeued\n", N);
	intrusive();
	container();
	return 0;
}
