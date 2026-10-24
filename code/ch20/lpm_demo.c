// SPDX-License-Identifier: MIT
/*
 * lpm_demo.c - a small IPv4 routing table in the LPM trie.
 * The output is deterministic; Section 20.6 quotes it.
 */
#include "lpm.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *names[] = { "default", "A", "B", "C", "D", "E" };

static struct lpm_key key(int a, int b, int c, int d, int len)
{
	struct lpm_key k = { .prefixlen = len,
			     .data = { a, b, c, d } };
	return k;
}

static void show(const struct lpm_node *n, int depth)
{
	char buf[32];

	if (!n)
		return;
	snprintf(buf, sizeof(buf), "%u.%u.%u.%u/%u", n->data[0], n->data[1],
		 n->data[2], n->data[3], n->prefixlen);
	printf("%*s%s%*s%s\n", 2 * depth, "", buf,
	       24 - 2 * depth - (int)strlen(buf), "",
	       n->flags & LPM_FLAG_IM ? "(intermediate)" : names[n->value]);
	show(n->child[0], depth + 1);
	show(n->child[1], depth + 1);
}

static void lookup(const struct lpm_trie *t, int a, int b, int c, int d)
{
	struct lpm_key k = key(a, b, c, d, 32);
	uint32_t *v = lpm_lookup(t, &k);

	printf("  %u.%u.%u.%u -> %s\n", a, b, c, d, v ? names[*v] : "none");
}

int main(void)
{
	struct lpm_trie t;
	struct lpm_key k, prev;

	lpm_init(&t, 4, 16);
	k = key(0, 0, 0, 0, 0);		lpm_update(&t, &k, 0, BPF_ANY);
	k = key(10, 0, 0, 0, 8);	lpm_update(&t, &k, 1, BPF_ANY);
	k = key(10, 1, 0, 0, 16);	lpm_update(&t, &k, 2, BPF_ANY);
	k = key(10, 1, 2, 0, 24);	lpm_update(&t, &k, 3, BPF_ANY);
	k = key(10, 1, 3, 0, 24);	lpm_update(&t, &k, 4, BPF_ANY);
	k = key(10, 200, 0, 0, 16);	lpm_update(&t, &k, 5, BPF_ANY);
	printf("%zu entries, %zu nodes:\n", t.n_entries, t.n_nodes);
	show(t.root, 1);

	printf("lookups:\n");
	lookup(&t, 10, 1, 3, 7);
	lookup(&t, 10, 1, 4, 1);
	lookup(&t, 10, 2, 0, 1);
	lookup(&t, 10, 200, 5, 5);
	lookup(&t, 192, 168, 1, 1);

	printf("iteration order:");
	for (int ret = lpm_get_next_key(&t, NULL, &k); ret == 0;
	     ret = lpm_get_next_key(&t, &prev, &k)) {
		printf(" %u.%u.%u.%u/%u", k.data[0], k.data[1], k.data[2],
		       k.data[3], k.prefixlen);
		prev = k;
	}
	printf("\n");

	k = key(10, 1, 2, 0, 24);
	assert(lpm_delete(&t, &k) == 0);
	printf("after deleting 10.1.2.0/24: %zu entries, %zu nodes:\n",
	       t.n_entries, t.n_nodes);
	show(t.root, 1);
	lpm_destroy(&t);
	return 0;
}
