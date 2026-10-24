// SPDX-License-Identifier: MIT
/*
 * test_graph.c - randomized tests for cfg.c, lockgraph.c and union_find.c.
 *
 *   ./test_graph [rounds] [seed]
 *
 * Every iterative, fixed-memory algorithm is checked against a simple
 * model that is allowed to recurse and to use as much memory as it likes:
 *   - check_cfg():   a recursive DFS that classifies the same edges;
 *   - cfg_scc():     brute-force mutual reachability;
 *   - add_dependency(): an adjacency matrix and a plain BFS for distances;
 *   - uf_*():        a label array that is relabelled on every union.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfg.h"
#include "lockgraph.h"
#include "union_find.h"

static unsigned long long rng_state;

static unsigned int rnd(unsigned int n)
{
	rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
	return (unsigned int)(rng_state >> 33) % n;
}

/* ---- random programs -------------------------------------------------- */

#define MAXN 200

static struct insn insns[MAXN];
static bool bad_jumps;			/* the last random_prog() has some */

static struct prog random_prog(int n, bool allow_bad_jumps)
{
	bad_jumps = false;
	for (int t = 0; t < n; t++) {
		unsigned int r = rnd(100);
		enum op op = r < 55 ? OP_NEXT : r < 80 ? OP_JCOND :
			     r < 92 ? OP_JA : OP_EXIT;

		if (t == n - 1 && (op == OP_NEXT || op == OP_JCOND))
			op = OP_EXIT;
		int target = (int)rnd(n);

		if (allow_bad_jumps && rnd(50) == 0) {
			target = rnd(2) ? n : -1;
			bad_jumps |= op == OP_JA || op == OP_JCOND;
		}
		insns[t] = (struct insn){ op, target - t - 1 };
	}
	return (struct prog){ n, insns };
}

/* ---- model 1: recursive DFS ------------------------------------------- */

static int color[MAXN];			/* 0 white, 1 grey, 2 black */
static unsigned char mkind[2 * MAXN];

struct merr { int err, from, to; };

static int model_dfs(const struct prog *p, int t, bool allow_loops,
		     struct merr *me)
{
	const struct insn *in = &p->insns[t];
	int targets[2], slots[2], n = 0;

	color[t] = 1;
	if (in->op == OP_NEXT || in->op == OP_JCOND) {
		targets[n] = t + 1;
		slots[n++] = 0;
	}
	if (in->op == OP_JA || in->op == OP_JCOND) {
		targets[n] = t + in->off + 1;
		slots[n++] = in->op == OP_JA ? 0 : 1;
	}
	for (int i = 0; i < n; i++) {
		int w = targets[i];

		if (w < 0 || w >= p->len) {
			*me = (struct merr){ -ERANGE, t, w };
			return -1;
		}
		if (color[w] == 0) {
			mkind[2 * t + slots[i]] = E_TREE;
			if (model_dfs(p, w, allow_loops, me))
				return -1;
		} else if (color[w] == 1) {
			mkind[2 * t + slots[i]] = E_BACK;
			if (!allow_loops) {
				*me = (struct merr){ -EINVAL, t, w };
				return -1;
			}
		} else {
			mkind[2 * t + slots[i]] = E_FWD_CROSS;
		}
	}
	color[t] = 2;
	return 0;
}

static void model_check_cfg(const struct prog *p, bool allow_loops,
			    struct merr *me)
{
	memset(color, 0, sizeof(color));
	memset(mkind, 0, sizeof(mkind));
	*me = (struct merr){ 0, -1, -1 };
	if (model_dfs(p, 0, allow_loops, me))
		return;
	for (int i = 0; i < p->len; i++)
		if (color[i] != 2) {
			*me = (struct merr){ -EINVAL, i, i };
			return;
		}
}

/* ---- model 2: reachability -------------------------------------------- */

static unsigned char reach[MAXN][MAXN];	/* reach[u][v]: path of >= 1 edge */

static void closure(const struct prog *p)
{
	int succ[2];

	memset(reach, 0, sizeof(reach));
	for (int u = 0; u < p->len; u++) {
		int n = cfg_successors(p, u, succ);

		for (int i = 0; i < n; i++)
			reach[u][succ[i]] = 1;
	}
	for (int k = 0; k < p->len; k++)
		for (int i = 0; i < p->len; i++)
			if (reach[i][k])
				for (int j = 0; j < p->len; j++)
					if (reach[k][j])
						reach[i][j] = 1;
}

static long n_cfg, n_loops_found, n_unreach, n_range, n_scc_loops;
static long n_po_acyclic, n_po_not_post;

static void test_cfg_once(int n)
{
	struct prog p = random_prog(n, rnd(4) == 0);
	unsigned char kind[2 * MAXN];
	struct cfg_report r = { .kind = kind };
	struct merr me;

	/* Privileged mode: loops allowed, every edge classified. */
	memset(kind, 0, sizeof(kind));
	check_cfg(&p, true, &r);
	model_check_cfg(&p, true, &me);
	assert(r.err == me.err && r.from == me.from && r.to == me.to);
	assert(r.max_stack <= n);
	if (r.err != -ERANGE)
		assert(memcmp(kind, mkind, 2 * n) == 0);
	if (r.err == -ERANGE || bad_jumps) {	/* maybe in dead code */
		n_range++;
		return;
	}
	if (r.err)
		n_unreach++;
	n_cfg++;

	/* A back edge exists iff a cycle is reachable from instruction 0. */
	bool back = false, cyc = false;

	for (int i = 0; i < 2 * n; i++)
		back |= kind[i] == E_BACK;
	closure(&p);
	for (int v = 0; v < n; v++)
		cyc |= (v == 0 || reach[0][v]) && reach[v][v];
	assert(back == cyc);

	/* Unprivileged mode: the first back edge is an error. */
	check_cfg(&p, false, &r);
	model_check_cfg(&p, false, &me);
	assert(r.err == me.err && r.from == me.from && r.to == me.to);
	if (back)
		n_loops_found++;

	/* Postorder: every reachable instruction exactly once. */
	int order[MAXN], pos[MAXN], cnt = cfg_postorder(&p, order);
	int nreach = 0;

	for (int v = 0; v < n; v++) {
		pos[v] = -1;
		nreach += v == 0 || reach[0][v];
	}
	assert(cnt == nreach);
	for (int i = 0; i < cnt; i++) {
		assert(pos[order[i]] == -1 && (order[i] == 0 || reach[0][order[i]]));
		pos[order[i]] = i;
	}
	if (!cyc) {		/* in a DAG, a true postorder puts v before u */
		bool bad = false;
		int succ[2];

		for (int u = 0; u < n; u++) {
			if (pos[u] < 0)
				continue;
			int k = cfg_successors(&p, u, succ);

			for (int i = 0; i < k; i++)
				bad |= pos[succ[i]] > pos[u];
		}
		n_po_acyclic++;
		n_po_not_post += bad;
	}

	/* SCCs against mutual reachability. */
	unsigned int scc[MAXN], comp[MAXN];
	int loops = cfg_scc(&p, scc, comp);
	int succ[2];

	assert(loops >= 0);
	n_scc_loops += loops;
	for (int u = 0; u < n; u++) {
		bool in_loop = reach[u][u];

		assert((scc[u] != 0) == in_loop);
		for (int v = 0; v < n; v++) {
			bool same = u == v || (reach[u][v] && reach[v][u]);

			assert((comp[u] == comp[v]) == same);
			if (scc[u] && scc[v])
				assert((scc[u] == scc[v]) == same);
		}
		/* Tarjan finishes a component after everything it reaches. */
		int k = cfg_successors(&p, u, succ);

		for (int i = 0; i < k; i++)
			assert(comp[succ[i]] <= comp[u]);
	}
}

/* A million-instruction straight line with one loop at the end. */
static void test_deep(void)
{
	const int n = 1000000;
	struct insn *big = malloc(n * sizeof(*big));
	struct cfg_report r = { 0 };
	unsigned int *scc = malloc(n * sizeof(*scc));

	assert(big && scc);
	for (int t = 0; t < n - 2; t++)
		big[t] = (struct insn){ OP_NEXT, 0 };
	big[n - 2] = (struct insn){ OP_JCOND, -2 };	/* back to n - 3 */
	big[n - 1] = (struct insn){ OP_EXIT, 0 };
	struct prog p = { n, big };

	assert(check_cfg(&p, true, &r) == 0 && r.max_stack == n);
	assert(check_cfg(&p, false, &r) == -EINVAL);
	assert(r.from == n - 2 && r.to == n - 3);
	assert(cfg_scc(&p, scc, NULL) == 1);
	assert(scc[n - 3] == 1 && scc[n - 2] == 1 && scc[0] == 0);
	printf("cfg: 1,000,000-instruction chain: explicit stack reached %d entries\n",
	       r.max_stack);
	free(big);
	free(scc);
}

/* ---- model 3: lock graph ---------------------------------------------- */

#define NCLASS 24

static struct lock_graph g;
static struct lock_class cls[NCLASS];
static unsigned char adj[NCLASS][NCLASS];
static const char *names[NCLASS];

static int model_dist(int from, int to)	/* edges on a shortest path, -1 none */
{
	int dist[NCLASS], q[NCLASS], h = 0, tl = 0;

	for (int i = 0; i < NCLASS; i++)
		dist[i] = -1;
	dist[from] = 0;
	q[tl++] = from;
	while (h < tl) {
		int u = q[h++];

		for (int v = 0; v < NCLASS; v++)
			if (adj[u][v] && dist[v] < 0) {
				dist[v] = dist[u] + 1;
				q[tl++] = v;
			}
	}
	return dist[to];
}

static long n_dep_ok, n_dep_cycle, n_dep_full;

static void test_lockgraph(int ops, unsigned int cq_bits)
{
	static char buf[NCLASS][8];
	struct lock_class *path[NCLASS];
	int len;

	lock_graph_init(&g, cq_bits);
	memset(adj, 0, sizeof(adj));
	for (int i = 0; i < NCLASS; i++) {
		snprintf(buf[i], sizeof(buf[i]), "L%d", i);
		names[i] = buf[i];
		lock_class_init(&cls[i], names[i]);
	}
	for (int k = 0; k < ops; k++) {
		/* Bias towards a few hub locks so that long paths form. */
		int a = rnd(4) ? (int)rnd(NCLASS) : (int)rnd(4);
		int b = rnd(4) ? (int)rnd(NCLASS) : (int)rnd(4);
		int d = model_dist(b, a);	/* a -> b closes a cycle iff b reaches a */
		bool was_disabled = g.disabled;
		int ret = add_dependency(&g, &cls[a], &cls[b], path, &len);

		if (was_disabled) {
			assert(ret == 0);
			continue;
		}
		if (ret == -ENOSPC) {
			assert(g.disabled && cq_bits < CQ_MAX_BITS);
			n_dep_full++;
			continue;
		}
		if (d < 0) {
			assert(ret == 0 && has_edge(&cls[a], &cls[b]));
			adj[a][b] = 1;
			n_dep_ok++;
			continue;
		}
		/* A cycle: the path must be a shortest one from b to a. */
		assert(ret == -EDEADLK && len == d + 1);
		assert(path[0] == &cls[b] && path[len - 1] == &cls[a]);
		for (int i = 0; i + 1 < len; i++)
			assert(adj[path[i] - cls][path[i + 1] - cls]);
		n_dep_cycle++;
	}
	if (cq_bits == CQ_MAX_BITS)
		assert(!g.disabled);
}

/* ---- model 4: union-find ---------------------------------------------- */

#define NUF 1000

static struct uf_node uf[NUF];
static int label[NUF];

/* Depth of x and its root, without changing the tree. */
static int depth(struct uf_node *x, struct uf_node **root)
{
	int h = 0;

	while (x->parent != x) {
		x = x->parent;
		h++;
	}
	*root = x;
	return h;
}

static void uf_invariants(void)
{
	int size[NUF] = { 0 };

	for (int i = 0; i < NUF; i++) {
		struct uf_node *x = &uf[i];

		struct uf_node *r;

		if (x->parent != x)
			assert(x->parent->rank > x->rank);
		assert(depth(x, &r) <= (int)r->rank);
		size[r - uf]++;
	}
	for (int i = 0; i < NUF; i++)
		if (uf[i].parent == &uf[i])
			assert(size[i] >= 1 << uf[i].rank);
}

static void test_union_find(int ops)
{
	for (int i = 0; i < NUF; i++) {
		uf_node_init(&uf[i]);
		label[i] = i;
	}
	for (int k = 0; k < ops; k++) {
		int a = rnd(NUF), b = rnd(NUF);

		if (rnd(3)) {
			uf_union(&uf[a], &uf[b]);
			int la = label[a], lb = label[b];

			for (int i = 0; i < NUF; i++)
				if (label[i] == lb)
					label[i] = la;
		} else {
			assert((uf_find(&uf[a]) == uf_find(&uf[b])) ==
			       (label[a] == label[b]));
		}
		if (k % 997 == 0)
			uf_invariants();
	}
	uf_invariants();
}

int main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 20000;

	rng_state = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;

	for (int i = 0; i < rounds; i++)
		test_cfg_once(1 + rnd(i % 10 ? 24 : MAXN));
	printf("cfg: %ld programs (%ld with loops, %ld with unreachable code), "
	       "%ld with bad jumps; %ld loops found by SCC\n",
	       n_cfg, n_loops_found, n_unreach, n_range, n_scc_loops);
	printf("cfg: postorder of %ld loop-free programs: %ld not a true DFS postorder\n",
	       n_po_acyclic, n_po_not_post);
	test_deep();

	for (int i = 0; i < rounds / 200 + 1; i++)
		test_lockgraph(400, CQ_MAX_BITS);
	for (int i = 0; i < rounds / 200 + 1; i++)
		test_lockgraph(400, 2);
	printf("lockgraph: %ld edges added, %ld cycles reported (all shortest), "
	       "%ld searches gave up on a full queue\n",
	       n_dep_ok, n_dep_cycle, n_dep_full);

	test_union_find(rounds);
	printf("union-find: %d operations on %d nodes agree with the model\n",
	       rounds, NUF);
	printf("all tests passed\n");
	return 0;
}
