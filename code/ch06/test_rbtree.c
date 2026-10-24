// SPDX-License-Identifier: MIT
/*
 * test_rbtree.c - randomized tests for rbtree.c, itree.c and ostree.c.
 *
 *   ./test_rbtree [operations] [seed]
 *
 * Each part inserts and erases random nodes from a fixed pool and keeps a
 * model (an "in the tree" flag per pool entry).  After every few steps it
 * checks the red-black rules, every parent pointer, the cached leftmost
 * node, the order of the keys, every augmented value, and the answers to
 * queries, against brute force over the model.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rbtree.h"
#include "itree.h"
#include "ostree.h"

#define POOL	600
#define EVERY	7		/* full check every EVERY operations */

static unsigned long long rng;

static unsigned long rnd(unsigned long n)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (unsigned long)(rng % n);
}

/* --- generic red-black checks -------------------------------------------- */

typedef void (*node_check_fn)(struct rb_node *n);

/* Returns the black height of the subtree at n. */
static int check_node(struct rb_node *n, struct rb_node *parent,
		      node_check_fn f, unsigned long *count)
{
	int lh, rh;

	if (!n)
		return 1;
	assert(((unsigned long)n & 3) == 0);
	assert(rb_parent(n) == parent);
	if (rb_is_red(n)) {
		assert(!n->rb_left || rb_is_black(n->rb_left));
		assert(!n->rb_right || rb_is_black(n->rb_right));
	}
	lh = check_node(n->rb_left, n, f, count);
	rh = check_node(n->rb_right, n, f, count);
	assert(lh == rh);
	if (f)
		f(n);
	(*count)++;
	return lh + rb_is_black(n);
}

static unsigned long check_tree(const struct rb_root *root, node_check_fn f)
{
	unsigned long count = 0;

	if (root->rb_node)
		assert(rb_is_black(root->rb_node));
	check_node(root->rb_node, NULL, f, &count);
	return count;
}

/* --- part 1: plain and cached trees, rotation counts ---------------------- */

struct item {
	struct rb_node rb;
	int key;
	int in;
};

static struct item items[POOL];
static int rotations;

static void count_propagate(struct rb_node *n, struct rb_node *stop)
{
	(void)n, (void)stop;
}

static void count_copy(struct rb_node *old, struct rb_node *new)
{
	(void)old, (void)new;
}

static void count_rotate(struct rb_node *old, struct rb_node *new)
{
	(void)old, (void)new;
	rotations++;
}

static const struct rb_augment_callbacks counting = {
	count_propagate, count_copy, count_rotate
};

static void item_insert(struct item *it, struct rb_root_cached *root,
			bool augmented)
{
	struct rb_node **link = &root->rb_root.rb_node, *parent = NULL;
	bool leftmost = true;

	while (*link) {
		parent = *link;
		if (it->key < rb_entry(parent, struct item, rb)->key) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = false;
		}
	}
	rb_link_node(&it->rb, parent, link);
	if (augmented)
		rb_insert_augmented_cached(&it->rb, root, leftmost, &counting);
	else
		rb_insert_color_cached(&it->rb, root, leftmost);
}

static void check_items(struct rb_root_cached *root, int model_count)
{
	int hist_tree[400] = { 0 }, hist_model[400] = { 0 };
	int n = 0, prev = -1;
	struct rb_node *rb;

	assert(check_tree(&root->rb_root, NULL) == (unsigned long)model_count);
	assert(root->rb_leftmost == rb_first(&root->rb_root));
	for (rb = rb_first(&root->rb_root); rb; rb = rb_next(rb)) {
		struct item *it = rb_entry(rb, struct item, rb);

		assert(it->in && it->key >= prev);
		prev = it->key;
		hist_tree[it->key]++;
		n++;
	}
	assert(n == model_count);
	for (rb = rb_last(&root->rb_root); rb; rb = rb_prev(rb))
		n--;
	assert(n == 0);
	for (int i = 0; i < POOL; i++)
		if (items[i].in)
			hist_model[items[i].key]++;
	assert(!memcmp(hist_tree, hist_model, sizeof(hist_tree)));
}

static void test_plain(long ops)
{
	struct rb_root_cached root = RB_ROOT_CACHED;
	int count = 0, max_ins = 0, max_del = 0;

	for (int i = 0; i < POOL; i++)
		items[i].key = (int)rnd(400);
	for (long op = 0; op < ops; op++) {
		struct item *it = &items[rnd(POOL)];
		bool augmented = op & 1;

		rotations = 0;
		if (!it->in) {
			item_insert(it, &root, augmented);
			it->in = 1;
			count++;
			if (augmented && rotations > max_ins)
				max_ins = rotations;
		} else {
			struct rb_node *next = rb_next(&it->rb);
			bool was_first = root.rb_leftmost == &it->rb;

			if (augmented) {
				rb_erase_augmented_cached(&it->rb, &root, &counting);
			} else {
				struct rb_node *ret = rb_erase_cached(&it->rb, &root);

				assert(ret == (was_first ? next : NULL));
			}
			RB_CLEAR_NODE(&it->rb);
			assert(RB_EMPTY_NODE(&it->rb));
			it->in = 0;
			count--;
			if (augmented && rotations > max_del)
				max_del = rotations;
		}
		if (op % EVERY == 0)
			check_items(&root, count);
	}
	check_items(&root, count);
	assert(max_ins <= 2 && max_del <= 3);
	printf("plain:   %ld ops ok (max rotations: insert %d, erase %d)\n",
	       ops, max_ins, max_del);
}

/* --- part 2: interval tree ------------------------------------------------ */

static struct it_node ivs[POOL];
static int iv_in[POOL];

static void check_it_node(struct rb_node *rb)
{
	struct it_node *n = rb_entry(rb, struct it_node, rb);
	unsigned long max = n->last;

	if (rb->rb_left) {
		struct it_node *l = rb_entry(rb->rb_left, struct it_node, rb);

		assert(l->start <= n->start);
		if (l->subtree_last > max)
			max = l->subtree_last;
	}
	if (rb->rb_right) {
		struct it_node *r = rb_entry(rb->rb_right, struct it_node, rb);

		assert(r->start >= n->start);
		if (r->subtree_last > max)
			max = r->subtree_last;
	}
	assert(n->subtree_last == max);
}

static long query_it(struct rb_root_cached *root, unsigned long a,
		     unsigned long b)
{
	static int seen[POOL], gen;
	long found = 0, expect = 0;
	unsigned long prev = 0;

	gen++;
	for (struct it_node *n = it_iter_first(root, a, b); n;
	     n = it_iter_next(n, a, b)) {
		int i = (int)(n - ivs);

		assert(iv_in[i] && seen[i] != gen);
		seen[i] = gen;
		assert(n->start <= b && n->last >= a);	/* overlaps */
		assert(n->start >= prev);		/* in order of start */
		prev = n->start;
		found++;
	}
	for (int i = 0; i < POOL; i++)
		if (iv_in[i] && ivs[i].start <= b && ivs[i].last >= a)
			expect++;
	assert(found == expect);
	return found;
}

static void test_itree(long ops)
{
	struct rb_root_cached root = RB_ROOT_CACHED;
	unsigned long count = 0;
	long queries = 0, hits = 0;

	for (int i = 0; i < POOL; i++) {
		ivs[i].start = rnd(1000);
		ivs[i].last = ivs[i].start + rnd(60);
	}
	for (long op = 0; op < ops; op++) {
		int i = (int)rnd(POOL);
		unsigned long a = rnd(1100), b = a + rnd(40);

		if (!iv_in[i]) {
			it_insert(&ivs[i], &root);
			count++;
		} else {
			it_remove(&ivs[i], &root);
			count--;
		}
		iv_in[i] = !iv_in[i];
		hits += query_it(&root, a, b);
		queries++;
		if (op % EVERY == 0) {
			assert(check_tree(&root.rb_root, check_it_node) == count);
			assert(root.rb_leftmost == rb_first(&root.rb_root));
		}
	}
	assert(check_tree(&root.rb_root, check_it_node) == count);
	printf("itree:   %ld ops ok (%ld queries, %ld matches)\n",
	       ops, queries, hits);
}

/* --- part 3: order-statistic tree ----------------------------------------- */

static struct os_node oss[POOL];
static int os_in[POOL];

static void check_os_node(struct rb_node *rb)
{
	struct os_node *n = rb_entry(rb, struct os_node, rb);
	unsigned long size = 1;

	if (rb->rb_left)
		size += rb_entry(rb->rb_left, struct os_node, rb)->size;
	if (rb->rb_right)
		size += rb_entry(rb->rb_right, struct os_node, rb)->size;
	assert(n->size == size);
}

static void check_os(struct rb_root *root, unsigned long count)
{
	unsigned long k = 0;

	assert(check_tree(root, check_os_node) == count);
	for (struct rb_node *rb = rb_first(root); rb; rb = rb_next(rb), k++) {
		struct os_node *n = rb_entry(rb, struct os_node, rb);

		assert(os_select(root, k) == n);
		assert(os_rank(n) == k);
	}
	assert(k == count && !os_select(root, count));
}

static void test_ostree(long ops)
{
	struct rb_root root = RB_ROOT;
	unsigned long count = 0;

	for (int i = 0; i < POOL; i++)
		oss[i].key = (long)rnd(300);
	for (long op = 0; op < ops; op++) {
		int i = (int)rnd(POOL);

		if (!os_in[i]) {
			os_insert(&oss[i], &root);
			count++;
		} else {
			os_erase(&oss[i], &root);
			count--;
		}
		os_in[i] = !os_in[i];
		if (count) {	/* a random select must agree with a count of smaller keys */
			unsigned long k = rnd(count), less = 0, le = 0;
			struct os_node *n = os_select(&root, k);

			for (int j = 0; j < POOL; j++) {
				if (os_in[j] && oss[j].key < n->key)
					less++;
				if (os_in[j] && oss[j].key <= n->key)
					le++;
			}
			assert(less <= k && k < le);
		}
		if (op % EVERY == 0)
			check_os(&root, count);
	}
	check_os(&root, count);
	printf("ostree:  %ld ops ok\n", ops);
}

int main(int argc, char **argv)
{
	long ops = argc > 1 ? atol(argv[1]) : 200000;

	rng = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	rng = rng * 2654435761ULL + 1;
	test_plain(ops);
	test_itree(ops);
	test_ostree(ops);
	return 0;
}
