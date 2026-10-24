// SPDX-License-Identifier: MIT
/*
 * lpm.c - a longest-prefix-match trie in the shape of the BPF LPM trie.
 * See lpm.h for what it leaves out.
 */
#include "lpm.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

void lpm_init(struct lpm_trie *t, size_t data_size, size_t max_entries)
{
	memset(t, 0, sizeof(*t));
	t->data_size = data_size;
	t->max_prefixlen = 8 * data_size;
	t->max_entries = max_entries;
}

static struct lpm_node *node_alloc(struct lpm_trie *t)
{
	struct lpm_node *n = calloc(1, sizeof(*n));

	if (n)
		t->n_nodes++;
	return n;
}

static void node_free(struct lpm_trie *t, struct lpm_node *n)
{
	if (n) {
		t->n_nodes--;
		free(n);	/* the kernel waits for an RCU grace period */
	}
}

/* How many leading bits of the node's prefix does the key match? */
static uint32_t match_len(const struct lpm_trie *t, const struct lpm_node *n,
			  const struct lpm_key *key)
{
	uint32_t limit = n->prefixlen < key->prefixlen ?
			 n->prefixlen : key->prefixlen;
	uint32_t len = 0;

	for (size_t i = 0; i < t->data_size && len < limit; i++) {
		uint8_t diff = n->data[i] ^ key->data[i];

		if (diff) {
			len += __builtin_clz(diff) - 24;  /* equal top bits */
			break;
		}
		len += 8;
	}
	return len < limit ? len : limit;
}

uint32_t *lpm_lookup(const struct lpm_trie *t, const struct lpm_key *key)
{
	struct lpm_node *node, *found = NULL;

	if (key->prefixlen > t->max_prefixlen)
		return NULL;
	for (node = t->root; node;) {
		uint32_t len = match_len(t, node, key);

		if (len == t->max_prefixlen) {	/* exact full-length match */
			found = node;
			break;
		}
		if (len < node->prefixlen)	/* diverged inside the prefix */
			break;
		if (!(node->flags & LPM_FLAG_IM))
			found = node;		/* best so far */
		node = node->child[extract_bit(key->data, node->prefixlen)];
	}
	return found ? &found->value : NULL;
}

/* Counts a new entry, or says why there is no room for it. */
static int check_add(struct lpm_trie *t, int flags)
{
	if (flags == BPF_EXIST)
		return -ENOENT;
	if (t->n_entries == t->max_entries)
		return -ENOSPC;
	t->n_entries++;
	return 0;
}

int lpm_update(struct lpm_trie *t, const struct lpm_key *key,
	       uint32_t value, int flags)
{
	struct lpm_node *node, *new_node, *im_node, **slot;
	uint32_t len = 0;
	int ret;

	if (flags < BPF_ANY || flags > BPF_EXIST ||
	    key->prefixlen > t->max_prefixlen)
		return -EINVAL;
	new_node = node_alloc(t);	/* allocate before taking the lock */
	if (!new_node)
		return -ENOMEM;
	new_node->prefixlen = key->prefixlen;
	new_node->value = value;
	memcpy(new_node->data, key->data, t->data_size);

	/* Walk down while the node's whole prefix matches and is shorter. */
	slot = &t->root;
	while ((node = *slot)) {
		len = match_len(t, node, key);
		if (node->prefixlen != len || node->prefixlen == key->prefixlen)
			break;
		slot = &node->child[extract_bit(key->data, node->prefixlen)];
	}

	if (!node) {				/* 1: an empty slot */
		ret = check_add(t, flags);
		if (ret)
			goto fail;
		*slot = new_node;
		return 0;
	}
	if (node->prefixlen == len) {		/* 2: same prefix: replace */
		if (!(node->flags & LPM_FLAG_IM)) {
			if (flags == BPF_NOEXIST) {
				ret = -EEXIST;
				goto fail;
			}
		} else {
			ret = check_add(t, flags);
			if (ret)
				goto fail;
		}
		new_node->child[0] = node->child[0];
		new_node->child[1] = node->child[1];
		*slot = new_node;
		node_free(t, node);
		return 0;
	}
	ret = check_add(t, flags);
	if (ret)
		goto fail;
	if (len == key->prefixlen) {		/* 3: new node is an ancestor */
		new_node->child[extract_bit(node->data, len)] = node;
		*slot = new_node;
		return 0;
	}
	im_node = node_alloc(t);		/* 4: split with a new node */
	if (!im_node) {
		t->n_entries--;
		ret = -ENOMEM;
		goto fail;
	}
	im_node->prefixlen = len;
	im_node->flags = LPM_FLAG_IM;
	memcpy(im_node->data, node->data, t->data_size);
	if (extract_bit(key->data, len)) {
		im_node->child[0] = node;
		im_node->child[1] = new_node;
	} else {
		im_node->child[0] = new_node;
		im_node->child[1] = node;
	}
	*slot = im_node;
	return 0;
fail:
	node_free(t, new_node);
	return ret;
}

int lpm_delete(struct lpm_trie *t, const struct lpm_key *key)
{
	struct lpm_node *node, *parent = NULL, **trim, **trim2;
	uint32_t len = 0;

	if (key->prefixlen > t->max_prefixlen)
		return -EINVAL;
	/* Find the node, its slot, its parent, and the parent's slot. */
	trim = trim2 = &t->root;
	while ((node = *trim)) {
		len = match_len(t, node, key);
		if (node->prefixlen != len || node->prefixlen == key->prefixlen)
			break;
		parent = node;
		trim2 = trim;
		trim = &node->child[extract_bit(key->data, node->prefixlen)];
	}
	if (!node || node->prefixlen != key->prefixlen ||
	    node->prefixlen != len || (node->flags & LPM_FLAG_IM))
		return -ENOENT;
	t->n_entries--;

	if (node->child[0] && node->child[1]) {
		/* Two children: keep the node as a branch point. */
		node->flags |= LPM_FLAG_IM;
		return 0;
	}
	if (parent && (parent->flags & LPM_FLAG_IM) &&
	    !node->child[0] && !node->child[1]) {
		/* A leaf under a branch point: the branch point goes too. */
		*trim2 = parent->child[parent->child[0] == node];
		node_free(t, parent);
		node_free(t, node);
		return 0;
	}
	/* Zero or one child: splice the node out. */
	*trim = node->child[0] ? node->child[0] : node->child[1];
	node_free(t, node);
	return 0;
}

static void copy_key(const struct lpm_trie *t, const struct lpm_node *n,
		     struct lpm_key *k)
{
	k->prefixlen = n->prefixlen;
	memcpy(k->data, n->data, t->data_size);
}

/* Leftmost real node in postorder under 'n' (every IM node has 2 kids). */
static const struct lpm_node *leftmost(const struct lpm_node *n)
{
	const struct lpm_node *found = NULL;

	while (n) {
		if (n->flags & LPM_FLAG_IM) {
			n = n->child[0];
		} else {
			found = n;
			n = n->child[0] ? n->child[0] : n->child[1];
		}
	}
	return found;
}

/*
 * Iterate in postorder: more specific prefixes before less specific ones.
 * A NULL or unknown key starts from the beginning.
 */
int lpm_get_next_key(const struct lpm_trie *t, const struct lpm_key *key,
		     struct lpm_key *next)
{
	const struct lpm_node *stack[8 * LPM_DATA_MAX + 1], *node;
	int sp = -1;
	uint32_t len;

	if (!t->root)
		return -ENOENT;
	if (!key || key->prefixlen > t->max_prefixlen)
		goto first;
	for (node = t->root; node;) {
		stack[++sp] = node;
		len = match_len(t, node, key);
		if (node->prefixlen != len || node->prefixlen == key->prefixlen)
			break;
		node = node->child[extract_bit(key->data, node->prefixlen)];
	}
	if (!node || node->prefixlen != len || (node->flags & LPM_FLAG_IM))
		goto first;
	/* Climb: the next node is in the right sibling subtree or above. */
	for (; sp > 0; sp--) {
		const struct lpm_node *parent = stack[sp - 1];

		if (parent->child[0] == stack[sp] && parent->child[1]) {
			copy_key(t, leftmost(parent->child[1]), next);
			return 0;
		}
		if (!(parent->flags & LPM_FLAG_IM)) {
			copy_key(t, parent, next);
			return 0;
		}
	}
	return -ENOENT;
first:
	copy_key(t, leftmost(t->root), next);
	return 0;
}

static void destroy(struct lpm_trie *t, struct lpm_node *n)
{
	if (n) {
		destroy(t, n->child[0]);
		destroy(t, n->child[1]);
		node_free(t, n);
	}
}

void lpm_destroy(struct lpm_trie *t)
{
	destroy(t, t->root);	/* recursion depth <= max_prefixlen + 1 */
	t->root = NULL;
}
