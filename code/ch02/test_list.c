// SPDX-License-Identifier: MIT
/*
 * test_list.c - randomized tests for list.h and list_nulls.h.
 *
 *   cc -O2 -Wall -DLIST_HARDENED -fsanitize=address,undefined \
 *      -o test_list test_list.c
 *   ./test_list [iterations] [seed]
 *
 * Every operation is mirrored in a model: one plain array of object ids
 * per list.  After each step every list is walked forwards and backwards
 * and compared with the model, and the link invariants are checked:
 *   list_head:   n->next->prev == n and n->prev->next == n for every node
 *   hlist:       *n->pprev == n for every node; unhashed nodes have no pprev
 *   hlist_nulls: every chain ends in the marker of its own bucket
 * The corruption checks are compiled in and must never fire, except in
 * the deliberate misuse tests at the end.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "list.h"
#include "list_nulls.h"

#define M	48		/* objects */
#define L	4		/* list_head lists */
#define B	8		/* hlist / hlist_nulls buckets */

struct obj {
	int id;
	int where;		/* list number, or -1 */
	struct list_head node;
	int hb;			/* hlist bucket, or -1 */
	struct hlist_node hnode;
	int nb;			/* hlist_nulls bucket, or -1 */
	struct hlist_nulls_node nnode;
};

static struct obj objs[M];
static struct list_head heads[L];
static int model[L][M], len[L];
static struct hlist_head hheads[B];
static int hmodel[B][M], hlen[B];
static struct hlist_nulls_head nheads[B];
static int nmodel[B][M], nlen[B];

static int rnd(int n)
{
	return rand() % n;
}

/* ---- model helpers -------------------------------------------------- */

static void m_insert(int *arr, int *n, int pos, int id)
{
	memmove(arr + pos + 1, arr + pos, (*n - pos) * sizeof(int));
	arr[pos] = id;
	(*n)++;
}

static int m_remove(int *arr, int *n, int id)
{
	for (int i = 0; i < *n; i++)
		if (arr[i] == id) {
			memmove(arr + i, arr + i + 1, (*n - i - 1) * sizeof(int));
			(*n)--;
			return i;
		}
	assert(0);
	return -1;
}

static int m_index(const int *arr, int n, int id)
{
	for (int i = 0; i < n; i++)
		if (arr[i] == id)
			return i;
	return -1;
}

static struct obj *pick(int on)	/* on: 1 = on a list, 0 = not, -1 = any */
{
	for (int tries = 0; tries < 4 * M; tries++) {
		struct obj *o = &objs[rnd(M)];

		if (on < 0 || (o->where >= 0) == on)
			return o;
	}
	return NULL;
}

static struct obj *nth(int l, int k)
{
	return &objs[model[l][k]];
}

/* ---- checks --------------------------------------------------------- */

static void check_lists(void)
{
	for (int l = 0; l < L; l++) {
		struct list_head *h = &heads[l], *p;
		struct obj *o;
		int i = 0;

		list_for_each_entry(o, h, node) {
			assert(i < len[l] && o->id == model[l][i]);
			assert(o->where == l);
			i++;
		}
		assert(i == len[l]);
		list_for_each_entry_reverse(o, h, node)
			assert(o->id == model[l][--i]);
		assert(i == 0);
		p = h;
		do {
			assert(p->next->prev == p && p->prev->next == p);
			p = p->next;
		} while (p != h);
		assert(list_count_nodes(h) == (size_t)len[l]);
		assert(list_empty(h) == (len[l] == 0));
		assert(list_is_singular(h) == (len[l] == 1));
		if (len[l]) {
			assert(list_first_entry(h, struct obj, node)->id ==
			       model[l][0]);
			assert(list_last_entry(h, struct obj, node)->id ==
			       model[l][len[l] - 1]);
		}
	}
}

static void check_hlists(void)
{
	for (int b = 0; b < B; b++) {
		struct obj *o;
		struct hlist_nulls_node *pos;
		int i = 0;

		assert(!hheads[b].first || hheads[b].first->pprev == &hheads[b].first);
		hlist_for_each_entry(o, &hheads[b], hnode) {
			assert(i < hlen[b] && o->id == hmodel[b][i]);
			assert(*o->hnode.pprev == &o->hnode);
			i++;
		}
		assert(i == hlen[b]);
		assert(hlist_empty(&hheads[b]) == (hlen[b] == 0));

		i = 0;
		hlist_nulls_for_each_entry(o, pos, &nheads[b], nnode) {
			assert(i < nlen[b] && o->id == nmodel[b][i]);
			assert(*o->nnode.pprev == &o->nnode);
			i++;
		}
		assert(i == nlen[b]);
		assert(is_a_nulls(pos) && get_nulls_value(pos) == (unsigned long)b);
	}
}

/* ---- list_head operations ------------------------------------------- */

static void list_step(void)
{
	int op = rnd(15), j = rnd(L), i = rnd(L);
	struct obj *a, *b;

	switch (op) {
	case 0:				/* list_add */
	case 1:				/* list_add_tail */
		if (!(a = pick(0)))
			break;
		if (op == 0)
			list_add(&a->node, &heads[j]);
		else
			list_add_tail(&a->node, &heads[j]);
		m_insert(model[j], &len[j], op ? len[j] : 0, a->id);
		a->where = j;
		break;
	case 2:				/* list_del */
	case 3:				/* list_del_init */
		if (!(a = pick(1)))
			break;
		m_remove(model[a->where], &len[a->where], a->id);
		if (op == 2) {
			list_del(&a->node);
			assert(a->node.next == LIST_POISON1);
		} else {
			list_del_init(&a->node);
			assert(list_empty(&a->node));
		}
		a->where = -1;
		break;
	case 4:				/* list_move */
	case 5:				/* list_move_tail */
		if (!(a = pick(1)))
			break;
		m_remove(model[a->where], &len[a->where], a->id);
		if (op == 4)
			list_move(&a->node, &heads[j]);
		else
			list_move_tail(&a->node, &heads[j]);
		m_insert(model[j], &len[j], op == 4 ? 0 : len[j], a->id);
		a->where = j;
		break;
	case 6:				/* list_replace */
		if (!(a = pick(1)) || !(b = pick(0)))
			break;
		{
			int l = a->where, k = m_index(model[l], len[l], a->id);

			list_replace(&a->node, &b->node);
			model[l][k] = b->id;
			b->where = l;
			a->where = -1;
		}
		break;
	case 7:				/* list_swap */
		if (!(a = pick(1)) || !(b = pick(1)) || a == b)
			break;
		{
			int la = a->where, ka = m_index(model[la], len[la], a->id);
			int lb = b->where, kb = m_index(model[lb], len[lb], b->id);

			list_swap(&a->node, &b->node);
			model[la][ka] = b->id;
			model[lb][kb] = a->id;
			a->where = lb;
			b->where = la;
		}
		break;
	case 8:				/* the four splices */
		if (i == j)
			break;
		{
			int kind = rnd(4), n = len[i];

			for (int k = 0; k < n; k++)
				objs[model[i][k]].where = j;
			if (kind == 0 || kind == 2) {	/* at the front */
				memmove(model[j] + n, model[j], len[j] * sizeof(int));
				memcpy(model[j], model[i], n * sizeof(int));
			} else {			/* at the back */
				memcpy(model[j] + len[j], model[i], n * sizeof(int));
			}
			len[j] += n;
			len[i] = 0;
			switch (kind) {
			case 0: list_splice(&heads[i], &heads[j]); break;
			case 1: list_splice_tail(&heads[i], &heads[j]); break;
			case 2: list_splice_init(&heads[i], &heads[j]); break;
			case 3: list_splice_tail_init(&heads[i], &heads[j]); break;
			}
			if (kind < 2)		/* plain splice leaves i stale */
				INIT_LIST_HEAD(&heads[i]);
			assert(list_empty(&heads[i]));
		}
		break;
	case 9:				/* list_cut_position, then splice */
	case 10:			/* list_cut_before, then splice */
		{
			LIST_HEAD(tmp);
			int k = rnd(len[i] + 1), n;
			struct list_head *entry;

			if (op == 9) {	/* cut up to and including entry k-1 */
				entry = k ? &nth(i, k - 1)->node : &heads[i];
				list_cut_position(&tmp, &heads[i], entry);
				n = k;
			} else {	/* cut everything before entry k */
				entry = k < len[i] ? &nth(i, k)->node : &heads[i];
				list_cut_before(&tmp, &heads[i], entry);
				n = k;
			}
			assert(list_count_nodes(&tmp) == (size_t)n);
			for (int x = 0; x < n; x++)
				objs[model[i][x]].where = j;
			int cut[M];

			memcpy(cut, model[i], n * sizeof(int));
			memmove(model[i], model[i] + n, (len[i] - n) * sizeof(int));
			len[i] -= n;
			memcpy(model[j] + len[j], cut, n * sizeof(int));
			len[j] += n;
			list_splice_tail_init(&tmp, &heads[j]);
		}
		break;
	case 11:			/* list_bulk_move_tail within one list */
		if (len[i] < 1)
			break;
		{
			int x = rnd(len[i]), y = x + rnd(len[i] - x), n = y - x + 1;
			int run[M];

			list_bulk_move_tail(&heads[i], &nth(i, x)->node,
					    &nth(i, y)->node);
			memcpy(run, model[i] + x, n * sizeof(int));
			memmove(model[i] + x, model[i] + y + 1,
				(len[i] - y - 1) * sizeof(int));
			memcpy(model[i] + len[i] - n, run, n * sizeof(int));
		}
		break;
	case 12:			/* list_rotate_left */
		list_rotate_left(&heads[i]);
		if (len[i] > 1) {
			int first = model[i][0];

			memmove(model[i], model[i] + 1, (len[i] - 1) * sizeof(int));
			model[i][len[i] - 1] = first;
		}
		break;
	case 13:			/* list_rotate_to_front */
		if (len[i] < 1)
			break;
		{
			int k = rnd(len[i]), rot[M];

			list_rotate_to_front(&nth(i, k)->node, &heads[i]);
			for (int x = 0; x < len[i]; x++)
				rot[x] = model[i][(k + x) % len[i]];
			memcpy(model[i], rot, len[i] * sizeof(int));
		}
		break;
	case 14:			/* delete odd ids while iterating */
		{
			struct obj *o, *n;

			list_for_each_entry_safe(o, n, &heads[i], node) {
				if (o->id & 1) {
					list_del(&o->node);
					m_remove(model[i], &len[i], o->id);
					o->where = -1;
				}
			}
		}
		break;
	}
}

/* ---- hlist and hlist_nulls operations ------------------------------- */

static void hlist_step(void)
{
	struct obj *a = &objs[rnd(M)];
	int b = rnd(B), op = rnd(4);

	if (a->hb < 0) {
		if (op == 0 || hlen[b] == 0) {		/* hlist_add_head */
			hlist_add_head(&a->hnode, &hheads[b]);
			m_insert(hmodel[b], &hlen[b], 0, a->id);
		} else {
			int k = rnd(hlen[b]);
			struct obj *t = &objs[hmodel[b][k]];

			if (op == 1) {			/* hlist_add_before */
				hlist_add_before(&a->hnode, &t->hnode);
				m_insert(hmodel[b], &hlen[b], k, a->id);
			} else {			/* hlist_add_behind */
				hlist_add_behind(&a->hnode, &t->hnode);
				m_insert(hmodel[b], &hlen[b], k + 1, a->id);
			}
		}
		a->hb = b;
	} else {
		m_remove(hmodel[a->hb], &hlen[a->hb], a->id);
		if (op & 1) {
			hlist_del_init(&a->hnode);
			assert(hlist_unhashed(&a->hnode));
			hlist_del_init(&a->hnode);	/* second time: no-op */
		} else {
			hlist_del(&a->hnode);
			assert(a->hnode.pprev == LIST_POISON2);
		}
		a->hb = -1;
	}

	a = &objs[rnd(M)];
	if (a->nb < 0) {
		hlist_nulls_add_head(&a->nnode, &nheads[b]);
		m_insert(nmodel[b], &nlen[b], 0, a->id);
		a->nb = b;
	} else {
		m_remove(nmodel[a->nb], &nlen[a->nb], a->id);
		hlist_nulls_del(&a->nnode);
		a->nb = -1;
	}
}

/* ---- deliberate misuse: the checks must catch these ----------------- */

static void misuse(void)
{
	LIST_HEAD(h);
	struct obj x = { .id = 0 }, y = { .id = 1 };
	long before = *list_corruptions();

	printf("deliberate misuse (each must be caught):\n");
	list_add(&x.node, &h);
	list_add(&x.node, &h);			/* double add */
	assert(*list_corruptions() == before + 1);
	list_del(&x.node);
	list_del(&x.node);			/* double delete */
	assert(*list_corruptions() == before + 2);
	list_add(&x.node, &h);
	list_add(&y.node, &h);
	x.node.prev = &x.node;			/* corrupt a back link */
	list_del(&y.node);
	assert(*list_corruptions() == before + 3);
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 200000;

	srand(argc > 2 ? atoi(argv[2]) : 1);
	for (int l = 0; l < L; l++)
		INIT_LIST_HEAD(&heads[l]);
	for (int b = 0; b < B; b++) {
		INIT_HLIST_HEAD(&hheads[b]);
		INIT_HLIST_NULLS_HEAD(&nheads[b], b);
	}
	for (int i = 0; i < M; i++) {
		objs[i].id = i;
		objs[i].where = objs[i].hb = objs[i].nb = -1;
		INIT_LIST_HEAD(&objs[i].node);
		INIT_HLIST_NODE(&objs[i].hnode);
	}
	for (long it = 0; it < iters; it++) {
		list_step();
		hlist_step();
		check_lists();
		check_hlists();
	}
	assert(*list_corruptions() == 0);
	printf("ok: %ld iterations, no corruption reported\n", iters);
	misuse();
	printf("ok: misuse caught\n");
	return 0;
}
