// SPDX-License-Identifier: MIT
/*
 * cfg.h - a toy instruction set whose control-flow graph is analysed the
 * way the BPF verifier analyses BPF programs: iterative depth-first search
 * with edge classification, a postorder walk, and strongly connected
 * components by a non-recursive Tarjan.  See cfg.c.
 */
#ifndef CFG_H
#define CFG_H

#include <stdbool.h>

enum op {
	OP_NEXT,	/* ordinary instruction: falls through to t + 1 */
	OP_JA,		/* unconditional jump to t + off + 1 */
	OP_JCOND,	/* conditional: falls through to t + 1 or jumps */
	OP_EXIT,	/* no successors */
};

struct insn {
	enum op op;
	int off;	/* jump offset, relative to the next instruction */
};

struct prog {
	int len;
	const struct insn *insns;
};

/* How the depth-first search classified an edge. */
enum edge_kind { E_NONE, E_TREE, E_BACK, E_FWD_CROSS };

/* Result of check_cfg(). */
struct cfg_report {
	int err;		/* 0, -EINVAL (loop, unreachable), -ERANGE */
	int from, to;		/* the offending edge or instruction */
	unsigned char *kind;	/* optional: kind[2*t + slot], slot 0 = fall-through/JA */
	int max_stack;		/* deepest the explicit stack got */
};

int cfg_successors(const struct prog *p, int t, int succ[2]);
int check_cfg(const struct prog *p, bool allow_loops, struct cfg_report *r);
int cfg_postorder(const struct prog *p, int *order);
int cfg_scc(const struct prog *p, unsigned int *scc, unsigned int *comp);

#endif
