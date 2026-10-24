// SPDX-License-Identifier: MIT
/*
 * stack_depth.c - Exercise 10.1: depth-first search on a 16 KiB stack.
 *
 *   ./stack_depth N iterative     check_cfg() on an N-instruction chain
 *   ./stack_depth N recursive     a textbook-style recursive DFS instead
 *
 * Both run in a thread whose stack is 16 KiB, the size of a kernel stack
 * on x86-64.  The recursive version needs one stack frame per instruction
 * on the path, so beyond some N it crashes (usually with SIGSEGV).  Find
 * that N on your machine, with and without optimization.
 */
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfg.h"

static struct prog prog;
static char *seen;
static int recursive;
static long result;

static long dfs_rec(int t)		/* visits t and everything below it */
{
	int succ[2], n = cfg_successors(&prog, t, succ);
	long cnt = 1;

	seen[t] = 1;
	for (int i = 0; i < n; i++)
		if (!seen[succ[i]])
			cnt += dfs_rec(succ[i]);
	return cnt;
}

static void *worker(void *arg)
{
	struct cfg_report r = { 0 };

	(void)arg;
	if (recursive) {
		result = dfs_rec(0);
	} else {
		check_cfg(&prog, true, &r);
		result = r.max_stack;
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 100000;
	size_t stack = 16384;
	pthread_attr_t attr;
	pthread_t th;

	recursive = argc > 2 && !strcmp(argv[2], "recursive");
	if (n < 2)
		n = 2;
	struct insn *in = calloc(n, sizeof(*in));

	seen = calloc(n, 1);
	if (!in || !seen)
		return 1;
	for (int t = 0; t < n - 1; t++)
		in[t] = (struct insn){ OP_NEXT, 0 };
	in[n - 1] = (struct insn){ OP_EXIT, 0 };
	prog = (struct prog){ n, in };

	if (stack < PTHREAD_STACK_MIN)
		stack = PTHREAD_STACK_MIN;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, stack);
	if (pthread_create(&th, &attr, worker, NULL))
		return 1;
	pthread_join(th, NULL);
	printf("%s DFS on %d instructions with a %zu-byte stack: %s %ld\n",
	       recursive ? "recursive" : "iterative", n, stack,
	       recursive ? "visited" : "explicit stack peaked at", result);
	free(in);
	free(seen);
	return 0;
}
