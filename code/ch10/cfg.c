// SPDX-License-Identifier: MIT
/*
 * cfg.c - control-flow-graph analysis in the style of the BPF verifier.
 *
 * Three walks over the graph of a toy program (cfg.h), none of them
 * recursive, each with all of its memory allocated before it starts:
 *
 *   check_cfg()     depth-first search with an explicit stack; the state of
 *                   each instruction encodes both its colour and which of
 *                   its edges have been labelled.  Finds back edges (loops),
 *                   unreachable instructions and jumps out of range.
 *   cfg_postorder() a second iterative style that pushes all successors at
 *                   once; the order it records is used for data-flow passes.
 *   cfg_scc()       strongly connected components by a non-recursive Tarjan
 *                   (with the simplification described by D. J. Pearce).
 *
 * What it leaves out on purpose: calls into other functions, instructions
 * that occupy two slots, indirect jumps (more than two successors), and
 * everything the verifier does after these passes.
 */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include "cfg.h"

/* Successors in the kernel's order: fall-through first, then the jump. */
int cfg_successors(const struct prog *p, int t, int succ[2])
{
	const struct insn *in = &p->insns[t];
	int n = 0;

	if (in->op == OP_NEXT || in->op == OP_JCOND)
		succ[n++] = t + 1;
	if (in->op == OP_JA || in->op == OP_JCOND)
		succ[n++] = t + in->off + 1;
	return n;
}

/* ---- 1. Depth-first search with mark encoding ------------------------- */

enum {
	DISCOVERED = 0x10,	/* on the stack */
	EXPLORED = 0x20,	/* finished */
	FALLTHROUGH = 1,	/* low bits: the last edge labelled so far */
	BRANCH = 2,
};

enum { DONE_EXPLORING = 0, KEEP_EXPLORING = 1 };

struct walk {
	const struct prog *p;
	bool allow_loops;
	int *state;		/* one mark per instruction */
	int *stack;		/* explicit DFS stack: instruction indices */
	int sp;
	struct cfg_report *r;
};

static void label(struct walk *wk, int t, int e, int kind)
{
	if (wk->r->kind)
		wk->r->kind[2 * t + (e == BRANCH)] = kind;
}

static int fail(struct walk *wk, int err, int from, int to)
{
	wk->r->err = err;
	wk->r->from = from;
	wk->r->to = to;
	return err;
}

/* Label edge t -> w of kind e (FALLTHROUGH or BRANCH). */
static int push_edge(struct walk *wk, int t, int w, int e)
{
	int *state = wk->state;

	if (state[t] >= (DISCOVERED | e))
		return DONE_EXPLORING;		/* labelled on an earlier visit */
	if (w < 0 || w >= wk->p->len)
		return fail(wk, -ERANGE, t, w);

	if (state[w] == 0) {			/* tree edge: descend */
		label(wk, t, e, E_TREE);
		state[t] = DISCOVERED | e;
		state[w] = DISCOVERED;
		wk->stack[wk->sp++] = w;	/* at most once per node */
		return KEEP_EXPLORING;
	}
	if ((state[w] & 0xF0) == DISCOVERED) {	/* back edge: a loop */
		label(wk, t, e, E_BACK);
		if (!wk->allow_loops)
			return fail(wk, -EINVAL, t, w);
	} else {				/* forward or cross edge */
		label(wk, t, e, E_FWD_CROSS);
	}
	state[t] = DISCOVERED | e;
	return DONE_EXPLORING;
}

/* Try the next unlabelled edge of t. */
static int visit_insn(struct walk *wk, int t)
{
	const struct insn *in = &wk->p->insns[t];
	int ret;

	switch (in->op) {
	case OP_EXIT:
		return DONE_EXPLORING;
	case OP_NEXT:
		return push_edge(wk, t, t + 1, FALLTHROUGH);
	case OP_JA:
		return push_edge(wk, t, t + in->off + 1, FALLTHROUGH);
	case OP_JCOND:
		ret = push_edge(wk, t, t + 1, FALLTHROUGH);
		if (ret)
			return ret;	/* descended, or failed */
		return push_edge(wk, t, t + in->off + 1, BRANCH);
	}
	return -EINVAL;
}

int check_cfg(const struct prog *p, bool allow_loops, struct cfg_report *r)
{
	struct walk wk = { .p = p, .allow_loops = allow_loops, .r = r };
	int ret = 0;

	r->err = 0;
	r->from = r->to = -1;
	r->max_stack = 0;
	/* All memory up front: one mark and one stack slot per instruction. */
	wk.state = calloc(p->len, sizeof(int));
	wk.stack = calloc(p->len, sizeof(int));
	if (!wk.state || !wk.stack) {
		ret = r->err = -ENOMEM;
		goto out;
	}

	wk.state[0] = DISCOVERED;
	wk.stack[0] = 0;
	wk.sp = 1;
	while (wk.sp > 0) {
		int t = wk.stack[wk.sp - 1];

		if (wk.sp > r->max_stack)
			r->max_stack = wk.sp;
		ret = visit_insn(&wk, t);
		if (ret == DONE_EXPLORING) {
			wk.state[t] = EXPLORED;
			wk.sp--;
		} else if (ret < 0) {
			goto out;
		}
	}

	for (int i = 0; i < p->len; i++) {
		if (wk.state[i] != EXPLORED) {
			ret = fail(&wk, -EINVAL, i, i);	/* unreachable */
			goto out;
		}
	}
	ret = 0;
out:
	free(wk.state);
	free(wk.stack);
	return ret;
}

/* ---- 2. Postorder, pushing all successors at once --------------------- */

/* Record reachable instructions in the order they finish; returns count. */
int cfg_postorder(const struct prog *p, int *order)
{
	int *state = calloc(p->len, sizeof(int));
	int *stack = calloc(p->len, sizeof(int));
	int sp = 1, cnt = 0, succ[2];

	if (!state || !stack) {
		free(state);
		free(stack);
		return -ENOMEM;
	}
	stack[0] = 0;
	do {
		int top = stack[sp - 1];

		state[top] |= DISCOVERED;
		if (state[top] & EXPLORED) {	/* second time on top: done */
			order[cnt++] = top;
			sp--;
			continue;
		}
		int n = cfg_successors(p, top, succ);

		for (int s = 0; s < n; s++) {
			if (!state[succ[s]]) {
				stack[sp++] = succ[s];
				state[succ[s]] |= DISCOVERED;
			}
		}
		state[top] |= EXPLORED;
	} while (sp);
	free(state);
	free(stack);
	return cnt;
}

/* ---- 3. Strongly connected components: Tarjan without recursion -------- */

/*
 * scc[t]: 0 if t is in no loop (a single instruction without a self edge),
 * otherwise a loop number >= 1, as in the kernel.  comp[t] (optional) numbers
 * every component, loops or not, in the order Tarjan finishes them.
 */
int cfg_scc(const struct prog *p, unsigned int *scc, unsigned int *comp)
{
	const unsigned int NOT_ON_STACK = UINT_MAX;
	const int n = p->len;
	unsigned int *pre = calloc(n, sizeof(*pre));	/* preorder number, 0 = new */
	unsigned int *low = calloc(n, sizeof(*low));	/* lowest pre reachable */
	int *stack = calloc(n, sizeof(*stack));		/* vertices not yet assigned */
	int *dfs = calloc(n, sizeof(*dfs));		/* replaces the call stack */
	unsigned int next_pre = 1, next_scc = 1, next_comp = 0;
	int stack_sz, dfs_sz, succ[2], cnt, ret = 0;

	if (!pre || !low || !stack || !dfs) {
		ret = -ENOMEM;
		goto out;
	}
	for (int i = 0; i < n; i++) {
		if (pre[i])
			continue;
		stack_sz = 0;
		dfs_sz = 1;
		dfs[0] = i;
dfs_continue:
		while (dfs_sz) {
			int w = dfs[dfs_sz - 1];

			if (pre[w] == 0) {		/* first visit: "call" */
				pre[w] = low[w] = next_pre++;
				stack[stack_sz++] = w;
			}
			cnt = cfg_successors(p, w, succ);
			for (int j = 0; j < cnt; j++) {
				if (pre[succ[j]]) {
					if (low[succ[j]] < low[w])
						low[w] = low[succ[j]];
				} else {
					dfs[dfs_sz++] = succ[j];
					goto dfs_continue;	/* "recurse" */
				}
			}
			if (low[w] < pre[w]) {		/* part of a loop above */
				dfs_sz--;		/* "return", stay on stack */
				goto dfs_continue;
			}
			/* w is the root of a component: number it only if it is a loop. */
			bool loop = stack[stack_sz - 1] != w;

			for (int j = 0; j < cnt; j++)
				if (succ[j] == w)
					loop = true;	/* self edge */
			int t;

			do {
				t = stack[--stack_sz];
				low[t] = NOT_ON_STACK;
				scc[t] = loop ? next_scc : 0;
				if (comp)
					comp[t] = next_comp;
			} while (t != w);
			next_scc += loop;
			next_comp++;
			dfs_sz--;
		}
	}
	ret = next_scc - 1;			/* number of loops */
out:
	free(pre);
	free(low);
	free(stack);
	free(dfs);
	return ret;
}
