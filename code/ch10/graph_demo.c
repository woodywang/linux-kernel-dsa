// SPDX-License-Identifier: MIT
/*
 * graph_demo.c - the examples used in Chapter 10.
 *
 *   1. check_cfg(), cfg_postorder() and cfg_scc() on a small loop;
 *   2. lockdep-style cycle reports, and what happens when the queue fills;
 *   3. merging overlapping CPU sets into scheduling domains with union-find.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "cfg.h"
#include "lockgraph.h"
#include "union_find.h"

/* ---- 1. a loop in a toy program --------------------------------------- */

static const struct insn loop_insns[] = {
	{ OP_NEXT, 0 },		/* 0: r1 = 10                 */
	{ OP_JCOND, 4 },	/* 1: if r2 == 0 goto 6       */
	{ OP_JCOND, 3 },	/* 2: if r1 == 0 goto 6       */
	{ OP_NEXT, 0 },		/* 3: r0 += r1                */
	{ OP_NEXT, 0 },		/* 4: r1 -= 1                 */
	{ OP_JA, -4 },		/* 5: goto 2                  */
	{ OP_EXIT, 0 },		/* 6: exit                    */
};

static void demo_cfg(void)
{
	static const char *kname[] = { "", "tree", "back", "forward/cross" };
	struct prog p = { 7, loop_insns };
	unsigned char kind[14] = { 0 };
	struct cfg_report r = { .kind = kind };
	unsigned int scc[7];
	int order[7], succ[2];

	const char *sep = "edges:";
	int printed = 0;

	check_cfg(&p, true, &r);
	for (int t = 0; t < p.len; t++) {
		int n = cfg_successors(&p, t, succ);

		for (int i = 0; i < n; i++) {
			int slot = loop_insns[t].op == OP_JCOND && i == 1;

			printf("%s %d->%d %s", sep, t, succ[i],
			       kname[kind[2 * t + slot]]);
			sep = ++printed % 4 ? "," : ",\n      ";
		}
	}
	printf("\nloops allowed:     ok, deepest stack %d of %d\n",
	       r.max_stack, p.len);
	check_cfg(&p, false, &r);
	printf("loops not allowed: back-edge from insn %d to %d\n", r.from, r.to);

	int n = cfg_postorder(&p, order);

	printf("postorder:");
	for (int i = 0; i < n; i++)
		printf(" %d", order[i]);
	cfg_scc(&p, scc, NULL);
	printf("\nscc:      ");
	for (int t = 0; t < p.len; t++)
		printf(" %u", scc[t]);
	printf("\n\n");
}

/* ---- 2. lock-dependency cycles ---------------------------------------- */

static struct lock_graph g;		/* big: keep it off the stack */

/* What a recursive depth-first search would report, for comparison. */
static int dfs_path(struct lock_class *from, struct lock_class *to,
		    struct lock_class **path, int depth)
{
	struct dep *e;

	path[depth] = from;
	if (from == to)
		return depth + 1;
	list_for_each_entry(e, &from->locks_after, entry) {
		int n = dfs_path(e->class, to, path, depth + 1);

		if (n)
			return n;
	}
	return 0;
}

static void print_cycle(struct lock_class **path, int len)
{
	for (int i = 0; i < len; i++)
		printf("%s -> ", path[i]->name);
	printf("%s", path[0]->name);
}

static void demo_lockdep(void)
{
	struct lock_class c[6], *path[16];
	const char *names[] = { "A", "B", "C", "D", "E", "F" };
	int len;

	lock_graph_init(&g, CQ_MAX_BITS);
	for (int i = 0; i < 6; i++)
		lock_class_init(&c[i], names[i]);
	/* A->B, B->C, C->D, D->E, then A->E directly. */
	add_dependency(&g, &c[0], &c[1], path, &len);
	add_dependency(&g, &c[1], &c[2], path, &len);
	add_dependency(&g, &c[2], &c[3], path, &len);
	add_dependency(&g, &c[3], &c[4], path, &len);
	add_dependency(&g, &c[0], &c[4], path, &len);

	int ret = add_dependency(&g, &c[4], &c[0], path, &len);

	printf("E -> A: %s, cycle ", ret == -EDEADLK ? "-EDEADLK" : "ok");
	print_cycle(path, len);
	len = dfs_path(&c[0], &c[4], path, 0);
	printf("\n        (depth-first would report ");
	print_cycle(path, len);
	printf(")\n");

	/* A hub with four neighbours, each with one more edge. */
	struct lock_class hub, x[4], y[4], top;
	static const char *xn[] = { "X0", "X1", "X2", "X3" };
	static const char *yn[] = { "Y0", "Y1", "Y2", "Y3" };

	for (unsigned int bits = 2; bits <= CQ_MAX_BITS; bits += CQ_MAX_BITS - 2) {
		lock_graph_init(&g, bits);
		lock_class_init(&hub, "H");
		lock_class_init(&top, "T");
		for (int i = 0; i < 4; i++) {
			lock_class_init(&x[i], xn[i]);
			lock_class_init(&y[i], yn[i]);
			add_dependency(&g, &hub, &x[i], path, &len);
			add_dependency(&g, &x[i], &y[i], path, &len);
		}
		ret = add_dependency(&g, &top, &hub, path, &len);
		printf("%4u-slot queue: T -> H: %s, most queued %d, checker %s\n",
		       1u << bits, ret == -ENOSPC ? "-ENOSPC" : "ok",
		       g.max_queue, g.disabled ? "off" : "on");
	}
	printf("\n");
}

/* ---- 3. scheduling domains from overlapping CPU sets ------------------ */

struct cpuset {
	const char *name;
	unsigned long cpus;		/* bit i = CPU i */
	struct uf_node node;		/* embedded, like a list_head */
};

static void print_cpus(unsigned long m)
{
	const char *sep = "";

	for (int i = 0; i < 64; i++) {
		if (!(m >> i & 1) || (i && m >> (i - 1) & 1))
			continue;		/* not the start of a run */
		int j = i;

		while (j + 1 < 64 && m >> (j + 1) & 1)
			j++;
		printf("%s%d-%d", sep, i, j);
		sep = ",";
	}
}

static void demo_cpuset(void)
{
	struct cpuset cs[] = {
		{ .name = "a", .cpus = 0x000f }, { .name = "b", .cpus = 0x003c },
		{ .name = "c", .cpus = 0x0f00 }, { .name = "d", .cpus = 0x0060 },
		{ .name = "e", .cpus = 0xf000 }, { .name = "f", .cpus = 0x1c00 },
	};
	int n = 6, ndoms = 0;

	for (int i = 0; i < n; i++)
		uf_node_init(&cs[i].node);
	for (int i = 0; i < n; i++)		/* merge every overlapping pair */
		for (int j = i + 1; j < n; j++)
			if (cs[i].cpus & cs[j].cpus)
				uf_union(&cs[i].node, &cs[j].node);

	for (int i = 0; i < n; i++) {
		if (uf_find(&cs[i].node) != &cs[i].node)
			continue;		/* not a root */
		unsigned long dom = 0;
		const char *sp = "";

		printf("domain %d: (", ndoms++);
		for (int j = 0; j < n; j++)
			if (uf_find(&cs[j].node) == &cs[i].node) {
				dom |= cs[j].cpus;
				printf("%s%s", sp, cs[j].name);
				sp = " ";
			}
		printf(") cpus ");
		print_cpus(dom);
		printf("\n");
	}
}

int main(void)
{
	demo_cfg();
	demo_lockdep();
	demo_cpuset();
	return 0;
}
