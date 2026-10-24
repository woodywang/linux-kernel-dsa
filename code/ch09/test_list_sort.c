// SPDX-License-Identifier: MIT
/*
 * test_list_sort.c - randomized tests for list_sort.c.
 *
 *   cc -O2 -Wall -Wextra -fsanitize=address,undefined \
 *      -o test_list_sort test_list_sort.c list_sort.c
 *   ./test_list_sort [rounds] [seed]
 *
 * Each round builds a list of random length (0 to 3000) whose keys are
 * drawn from a small range, so that there are many ties, and sorts it with
 * a three-way or a boolean comparison.  The model is an array of
 * (key, input position) pairs sorted by qsort() on both fields, which is
 * exactly what a stable sort must produce.  After sorting, the list is
 * walked forwards and backwards and compared with the model, and every
 * node's links are checked.  The comparison function also checks that a
 * always came before b in the input, as list_sort() promises.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "list_sort.h"

struct item {
	int key;
	int pos;			/* position in the input */
	struct list_head node;
};

static unsigned long ncmp;

static int cmp3(void *priv, const struct list_head *a,
		const struct list_head *b)
{
	const struct item *x = list_entry(a, struct item, node);
	const struct item *y = list_entry(b, struct item, node);

	(void)priv;
	ncmp++;
	assert(x->pos < y->pos);	/* a is always the earlier element */
	return (x->key > y->key) - (x->key < y->key);
}

static int cmp_bool(void *priv, const struct list_head *a,
		    const struct list_head *b)
{
	(void)priv;
	ncmp++;
	return list_entry(a, struct item, node)->key >
	       list_entry(b, struct item, node)->key;
}

static int model_cmp(const void *p, const void *q)
{
	const struct item *x = p, *y = q;

	if (x->key != y->key)
		return x->key < y->key ? -1 : 1;
	return x->pos < y->pos ? -1 : x->pos > y->pos;
}

int main(int argc, char **argv)
{
	long rounds = argc > 1 ? atol(argv[1]) : 2000;
	unsigned seed = argc > 2 ? (unsigned)atoi(argv[2]) : 1;
	unsigned long nodes = 0;

	srand(seed);
	for (long r = 0; r < rounds; r++) {
		int n = rand() % 4 == 0 ? rand() % 10 : rand() % 3000;
		int range = 1 + rand() % (rand() % 2 ? 8 : 100000);
		struct item *items = malloc((size_t)(n + 1) * sizeof(*items));
		struct item *model = malloc((size_t)(n + 1) * sizeof(*model));
		struct list_head head, *p;
		int i;

		assert(items && model);
		INIT_LIST_HEAD(&head);
		for (i = 0; i < n; i++) {
			items[i].key = rand() % range;
			items[i].pos = i;
			list_add_tail(&items[i].node, &head);
			model[i] = items[i];
		}
		qsort(model, (size_t)n, sizeof(*model), model_cmp);

		list_sort(NULL, &head, r % 2 ? cmp_bool : cmp3);

		i = 0;			/* forwards, with link checks */
		for (p = head.next; p != &head; p = p->next, i++) {
			struct item *it = list_entry(p, struct item, node);

			assert(i < n);
			assert(p->next->prev == p && p->prev->next == p);
			assert(it->key == model[i].key && it->pos == model[i].pos);
		}
		assert(i == n);
		for (p = head.prev; p != &head; p = p->prev)	/* backwards */
			assert(list_entry(p, struct item, node)->pos ==
			       model[--i].pos);
		assert(i == 0);
		nodes += (unsigned long)n;
		free(items);
		free(model);
	}
	printf("test_list_sort: %ld lists, %lu nodes, stable order checked, "
	       "seed %u\n", rounds, nodes, seed);
	return 0;
}
