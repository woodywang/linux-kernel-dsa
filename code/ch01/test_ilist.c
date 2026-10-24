// SPDX-License-Identifier: MIT
/*
 * test_ilist.c - randomized test: every object is on two lists; each step
 * adds, removes or moves objects, and the lists are compared with a simple
 * array model after every step.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -o test_ilist test_ilist.c
 *   ./test_ilist [iterations] [seed]
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "ilist.h"

#define M 64

struct obj {
	int id;
	struct list_head a, b;
	int on_a, on_b;		/* model: position key, 0 = not on list */
};

static struct obj objs[M];
static struct list_head A = LIST_HEAD_INIT(A), B = LIST_HEAD_INIT(B);
static int seq;

/* Check a list against the model: same members, in insertion order. */
static void check(struct list_head *h, int which)
{
	struct obj *o;
	int n = 0, last = 0, expect = 0;

	for (int i = 0; i < M; i++)
		expect += which ? objs[i].on_b != 0 : objs[i].on_a != 0;
	if (which) {
		list_for_each_entry(o, h, b) {
			assert(o->on_b > last);
			last = o->on_b;
			n++;
		}
	} else {
		list_for_each_entry(o, h, a) {
			assert(o->on_a > last);
			last = o->on_a;
			n++;
		}
	}
	assert(n == expect);
	assert((n == 0) == list_empty(h));
	/* walking backwards must visit the same number of nodes */
	n = 0;
	for (struct list_head *p = h->prev; p != h; p = p->prev)
		n++;
	assert(n == expect);
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 1000000;
	struct obj *o, *tmp;

	srand(argc > 2 ? atoi(argv[2]) : 1);
	for (int i = 0; i < M; i++) {
		objs[i].id = i;
		list_init(&objs[i].a);
		list_init(&objs[i].b);
	}
	for (long it = 0; it < iters; it++) {
		struct obj *x = &objs[rand() % M];
		int op = rand() % 5;

		if (op == 0 && !x->on_a) {
			list_add_tail(&x->a, &A);
			x->on_a = ++seq;
		} else if (op == 1 && !x->on_b) {
			list_add_tail(&x->b, &B);
			x->on_b = ++seq;
		} else if (op == 2 && x->on_a) {
			list_del(&x->a);
			x->on_a = 0;
		} else if (op == 3 && x->on_b) {
			list_del(&x->b);
			x->on_b = 0;
		} else if (op == 4) {
			/* drop every object with an odd id from A while iterating */
			list_for_each_entry_safe(o, tmp, &A, a) {
				if (o->id & 1) {
					list_del(&o->a);
					o->on_a = 0;
				}
			}
		}
		check(&A, 0);
		check(&B, 1);
	}
	printf("ok: %ld iterations\n", iters);
	return 0;
}
