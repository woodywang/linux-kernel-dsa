// SPDX-License-Identifier: MIT
/*
 * proctree.c - a process tree in "left-child, right-sibling" form, the way
 * struct task_struct links tasks: each task heads a list of its children
 * (children) and sits on its parent's list (sibling).
 *
 * Shows: a pre-order walk with no recursion and no stack (the parent
 * pointer is the way back up), and re-parenting all children of an exiting
 * task with one splice.
 *
 * What it leaves out on purpose: threads, locking (the kernel holds
 * tasklist_lock), and the separate "real parent" used by ptrace.
 *
 *   cc -O2 -Wall -o proctree proctree.c && ./proctree
 */
#include <stdio.h>
#include "list.h"

struct task {
	int pid;
	struct task *parent;
	struct list_head children;	/* head: my children, oldest first */
	struct list_head sibling;	/* node: my place in parent->children */
};

#define NTASKS 16
static struct task tasks[NTASKS];
static int ntasks;

static struct task *fork_task(struct task *parent, int pid)
{
	struct task *t = &tasks[ntasks++];

	t->pid = pid;
	t->parent = parent;
	INIT_LIST_HEAD(&t->children);
	INIT_LIST_HEAD(&t->sibling);
	if (parent)		/* new children go to the tail */
		list_add_tail(&t->sibling, &parent->children);
	return t;
}

/* Pre-order walk without recursion and without an explicit stack. */
static void walk_tree(struct task *root, void (*visit)(struct task *, int))
{
	struct task *t = root;
	int depth = 0;

	for (;;) {
		visit(t, depth);
		if (!list_empty(&t->children)) {		/* down */
			t = list_first_entry(&t->children, struct task,
					     sibling);
			depth++;
			continue;
		}
		while (t != root &&				/* up */
		       list_is_last(&t->sibling, &t->parent->children)) {
			t = t->parent;
			depth--;
		}
		if (t == root)
			return;
		t = list_next_entry(t, sibling);		/* right */
	}
}

static void print_task(struct task *t, int depth)
{
	printf("%*s%d\n", 2 * depth + 2, "", t->pid);
}

/* A task exits: its children move to @reaper, all at once. */
static void exit_task(struct task *t, struct task *reaper)
{
	struct task *child;

	list_for_each_entry(child, &t->children, sibling)
		child->parent = reaper;
	list_splice_tail_init(&t->children, &reaper->children);
	list_del_init(&t->sibling);
	t->parent = NULL;
}

int main(void)
{
	struct task *init = fork_task(NULL, 1);
	struct task *sshd = fork_task(init, 100);
	struct task *cron = fork_task(init, 200);
	struct task *bash = fork_task(sshd, 101);

	fork_task(cron, 201);
	fork_task(bash, 102);
	fork_task(bash, 103);
	fork_task(sshd, 104);

	printf("process tree:\n");
	walk_tree(init, print_task);
	printf("after pid %d exits:\n", sshd->pid);
	exit_task(sshd, init);
	walk_tree(init, print_task);
	return 0;
}
