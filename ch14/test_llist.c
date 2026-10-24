// SPDX-License-Identifier: MIT
/*
 * test_llist.c - randomized single-threaded test of llist.h against a
 * plain array used as a stack.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -o test_llist test_llist.c
 *   ./test_llist [steps] [seed]
 *
 * Checks the return values of llist_add()/llist_add_batch() ("was the
 * list empty?"), that llist_del_first() pops the newest node, and that
 * llist_del_all() followed by llist_reverse_order() yields the nodes
 * oldest first.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "container_of.h"
#include "llist.h"

#define N	512

struct item {
	struct llist_node node;
	int id;
};

static uint64_t rng;

static uint64_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

static void fail(const char *what, long step)
{
	fprintf(stderr, "step %ld: %s\n", step, what);
	exit(1);
}

int main(int argc, char **argv)
{
	long steps = argc > 1 ? atol(argv[1]) : 200000;
	static struct item it[N];
	int stack[N], free_ids[N], depth = 0, nfree = N;
	struct llist_head head;

	rng = (argc > 2 ? strtoull(argv[2], NULL, 0) : 1) *
	      0x9E3779B97F4A7C15ull + 1;
	init_llist_head(&head);
	for (int i = 0; i < N; i++) {
		it[i].id = i;
		free_ids[i] = N - 1 - i;
	}

	for (long step = 0; step < steps; step++) {
		unsigned op = (unsigned)(rnd() % 10);

		if (op < 5 && nfree) {			/* add 1..4 */
			int n = (int)(rnd() % 4) + 1, ids[4];
			bool was_empty;

			if (n > nfree)
				n = nfree;
			for (int i = 0; i < n; i++)
				ids[i] = free_ids[--nfree];
			for (int i = n - 1; i > 0; i--)	/* newest first */
				it[ids[i]].node.next = &it[ids[i - 1]].node;
			was_empty = n == 1 ? llist_add(&it[ids[0]].node, &head)
				: llist_add_batch(&it[ids[n - 1]].node,
						  &it[ids[0]].node, &head);
			if (was_empty != (depth == 0))
				fail("wrong 'was empty' result", step);
			for (int i = 0; i < n; i++)
				stack[depth++] = ids[i];
		} else if (op < 8) {			/* del_first */
			struct llist_node *n = llist_del_first(&head);

			if (!depth) {
				if (n)
					fail("del_first on an empty list", step);
				continue;
			}
			if (!n || container_of(n, struct item, node)->id !=
				  stack[depth - 1])
				fail("del_first did not pop the newest", step);
			free_ids[nfree++] = stack[--depth];
		} else {				/* del_all + reverse */
			struct llist_node *n, *list;
			int i = 0;

			list = llist_reverse_order(llist_del_all(&head));
			llist_for_each(n, list) {
				if (i >= depth ||
				    container_of(n, struct item, node)->id != stack[i])
					fail("del_all/reverse order wrong", step);
				i++;
			}
			if (i != depth)
				fail("del_all lost nodes", step);
			while (depth)
				free_ids[nfree++] = stack[--depth];
		}
		if (llist_empty(&head) != (depth == 0))
			fail("llist_empty() wrong", step);
	}
	printf("test_llist: %ld random operations agree with the model\n",
	       steps);
	return 0;
}
