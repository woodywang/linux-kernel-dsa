// SPDX-License-Identifier: MIT
/*
 * test_plist.c - randomized tests for plist.c.
 *
 *   cc -O2 -Wall -Wextra -fsanitize=address,undefined \
 *      -o test_plist test_plist.c plist.c
 *   ./test_plist [steps] [seed]
 *
 * Nodes are added, deleted and requeued at random, with priorities drawn
 * from a small range so that most priorities have several nodes.  The
 * model is an array of node ids in the order a priority list must keep
 * them: by priority, and first-come first-served among equals.  After
 * every step the plist is walked and compared with the model, and
 * plist_check() verifies the two-tier structure.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plist.h"

#define M	64		/* nodes */

static struct plist_node nodes[M];
static int model[M], mlen;

static int id_of(const struct plist_node *n)
{
	return (int)(n - nodes);
}

static void m_add(int id)
{
	int i = mlen;		/* after every node of equal or higher priority */

	while (i > 0 && nodes[model[i - 1]].prio > nodes[id].prio)
		i--;
	memmove(model + i + 1, model + i, (size_t)(mlen - i) * sizeof(int));
	model[i] = id;
	mlen++;
}

static void m_del(int id)
{
	for (int i = 0; i < mlen; i++)
		if (model[i] == id) {
			memmove(model + i, model + i + 1,
				(size_t)(mlen - i - 1) * sizeof(int));
			mlen--;
			return;
		}
	assert(!"not in model");
}

static void compare(const struct plist_head *head)
{
	const struct plist_node *n;
	int i = 0;

	assert(plist_check(head));
	list_for_each_entry(n, &head->node_list, node_list)
		assert(i < mlen && id_of(n) == model[i++]);
	assert(i == mlen);
	if (mlen) {
		assert(id_of(plist_first(head)) == model[0]);
		assert(id_of(plist_last(head)) == model[mlen - 1]);
	} else {
		assert(plist_head_empty(head));
	}
}

int main(int argc, char **argv)
{
	long steps = argc > 1 ? atol(argv[1]) : 300000;
	unsigned seed = argc > 2 ? (unsigned)atoi(argv[2]) : 1;
	struct plist_head head;
	int range = 6;

	srand(seed);
	plist_head_init(&head);
	for (int i = 0; i < M; i++)
		plist_node_init(&nodes[i], 0);

	for (long s = 0; s < steps; s++) {
		int id = rand() % M, op = rand() % 3;

		if (s % 50000 == 0)
			range = range == 6 ? 40 : 6;
		if (plist_node_empty(&nodes[id])) {	/* add */
			plist_node_init(&nodes[id], rand() % range - range / 2);
			plist_add(&nodes[id], &head);
			m_add(id);
		} else if (op == 0) {			/* requeue */
			plist_requeue(&nodes[id], &head);
			m_del(id);
			m_add(id);
		} else {				/* delete */
			plist_del(&nodes[id], &head);
			m_del(id);
			assert(plist_node_empty(&nodes[id]));
			assert(list_empty(&nodes[id].prio_list));
		}
		compare(&head);
	}
	printf("test_plist: %ld steps, %d nodes left, seed %u\n",
	       steps, mlen, seed);
	return 0;
}
