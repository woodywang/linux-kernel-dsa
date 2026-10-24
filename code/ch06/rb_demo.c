// SPDX-License-Identifier: MIT
/*
 * rb_demo.c - the demo whose output Chapter 6 quotes.
 *
 *   0. a caller-written insert loop and an in-order walk;
 *   1. how often insert and erase rotate, counted with a rotate callback;
 *   2. a small interval tree (file page ranges) and one overlap query;
 *   3. order statistics: median and rank with subtree sizes.
 *
 * The output is deterministic (fixed seed).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "rbtree.h"
#include "itree.h"
#include "ostree.h"

static unsigned long long rng = 42;

static unsigned long rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (unsigned long)rng;
}

/* --- 1. rotations per operation ------------------------------------------ */

struct item {
	struct rb_node rb;
	unsigned long key;
};

static int rotations;

static void no_propagate(struct rb_node *n, struct rb_node *stop)
{
	(void)n, (void)stop;
}

static void no_copy(struct rb_node *old, struct rb_node *new)
{
	(void)old, (void)new;
}

static void count_rotate(struct rb_node *old, struct rb_node *new)
{
	(void)old, (void)new;
	rotations++;
}

static const struct rb_augment_callbacks counting = {
	no_propagate, no_copy, count_rotate
};

/* The caller-written insert loop: find the link, then hand over. */
static void item_insert(struct item *new, struct rb_root *root)
{
	struct rb_node **link = &root->rb_node, *parent = NULL;

	while (*link) {
		struct item *this = rb_entry(*link, struct item, rb);

		parent = *link;
		if (new->key < this->key)
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}
	rb_link_node(&new->rb, parent, link);
	rb_insert_color(&new->rb, root);
}

/* The same loop, but rebalancing through a rotate callback that counts. */
static void item_insert_counted(struct item *new, struct rb_root *root)
{
	struct rb_node **link = &root->rb_node, *parent = NULL;

	while (*link) {
		parent = *link;
		if (new->key < rb_entry(parent, struct item, rb)->key)
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}
	rb_link_node(&new->rb, parent, link);
	rb_insert_augmented(&new->rb, root, &counting);
}

static void small_demo(void)
{
	static const unsigned long keys[] = { 50, 20, 80, 10, 30, 70, 90, 25 };
	struct item items[8];
	struct rb_root root = RB_ROOT;

	for (int i = 0; i < 8; i++) {
		items[i].key = keys[i];
		item_insert(&items[i], &root);
	}
	printf("in order:");
	for (struct rb_node *rb = rb_first(&root); rb; rb = rb_next(rb))
		printf(" %lu", rb_entry(rb, struct item, rb)->key);
	printf("\n");
}

static int height(const struct rb_node *n)
{
	int l, r;

	if (!n)
		return 0;
	l = height(n->rb_left);
	r = height(n->rb_right);
	return 1 + (l > r ? l : r);
}

static void rotation_counts(int n)
{
	struct item *items = calloc(n, sizeof(*items));
	struct rb_root root = RB_ROOT;
	long ins[3] = { 0 }, del[4] = { 0 };

	for (int i = 0; i < n; i++) {
		items[i].key = rnd();
		rotations = 0;
		item_insert_counted(&items[i], &root);
		ins[rotations]++;
	}
	printf("%d random inserts: height %d (limit 2*log2(n+1) = %.1f)\n",
	       n, height(root.rb_node), 2 * log2(n + 1.0));
	printf("  rotations per insert: 0: %4.1f%%  1: %4.1f%%  2: %4.1f%%\n",
	       100.0 * ins[0] / n, 100.0 * ins[1] / n, 100.0 * ins[2] / n);
	int *order = malloc(n * sizeof(*order));

	for (int i = 0; i < n; i++)
		order[i] = i;
	for (int i = n - 1; i > 0; i--) {		/* shuffle: erase in random order */
		int j = (int)(rnd() % (i + 1)), t = order[i];

		order[i] = order[j];
		order[j] = t;
	}
	for (int i = 0; i < n; i++) {
		rotations = 0;
		rb_erase_augmented(&items[order[i]].rb, &root, &counting);
		del[rotations]++;
	}
	free(order);
	printf("  rotations per erase:  0: %4.1f%%  1: %4.1f%%  2: %4.1f%%  3: %4.1f%%\n",
	       100.0 * del[0] / n, 100.0 * del[1] / n, 100.0 * del[2] / n,
	       100.0 * del[3] / n);
	free(items);
}

/* --- 2. an interval tree of file page ranges ----------------------------- */

static void print_it(const struct rb_node *rb, int depth)
{
	const struct it_node *n;

	if (!rb)
		return;
	n = rb_entry(rb, const struct it_node, rb);
	printf("  %*s[%lu,%lu] %s, subtree_last %lu\n", 2 * depth, "",
	       n->start, n->last, rb_is_red(rb) ? "red" : "black",
	       n->subtree_last);
	print_it(rb->rb_left, depth + 1);
	print_it(rb->rb_right, depth + 1);
}

static void interval_demo(void)
{
	static const unsigned long r[][2] = {
		{ 0, 9 }, { 4, 7 }, { 10, 19 }, { 12, 30 },
		{ 20, 24 }, { 31, 40 }, { 15, 16 }, { 35, 39 },
	};
	struct it_node nodes[8];
	struct rb_root_cached root = RB_ROOT_CACHED;

	for (int i = 0; i < 8; i++) {
		nodes[i].start = r[i][0];
		nodes[i].last = r[i][1];
		it_insert(&nodes[i], &root);
	}
	printf("interval tree (pre-order):\n");
	print_it(root.rb_root.rb_node, 0);
	printf("overlapping [17,21]:");
	for (struct it_node *n = it_iter_first(&root, 17, 21); n;
	     n = it_iter_next(n, 17, 21))
		printf(" [%lu,%lu]", n->start, n->last);
	printf("\n");
}

/* --- 3. order statistics ------------------------------------------------- */

static void order_demo(void)
{
	enum { N = 1001 };
	static struct os_node nodes[N];
	struct rb_root root = RB_ROOT;
	struct os_node *median;

	for (int i = 0; i < N; i++) {
		nodes[i].key = (long)(rnd() % 100000);
		os_insert(&nodes[i], &root);
	}
	median = os_select(&root, N / 2);
	printf("%d keys: median %ld (rank %lu); first key inserted, %ld, has rank %lu\n",
	       N, median->key, os_rank(median), nodes[0].key, os_rank(&nodes[0]));
}

int main(void)
{
	printf("sizeof(struct rb_node) = %zu, alignment %zu\n",
	       sizeof(struct rb_node), _Alignof(struct rb_node));
	small_demo();
	rotation_counts(100000);
	interval_demo();
	order_demo();
	return 0;
}
