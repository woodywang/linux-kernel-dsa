// SPDX-License-Identifier: MIT
/*
 * lpm.h - a longest-prefix-match trie in the shape of the BPF LPM trie map
 * (BPF_MAP_TYPE_LPM_TRIE), for Chapter 20.
 *
 * A binary trie with path compression: every node stores a whole prefix
 * (its bits and its length), and a node's children are chosen by the first
 * bit after its prefix.  Nodes that exist only to split a path are marked
 * intermediate and carry no value.
 *
 * What it leaves out on purpose: RCU and the trie-wide lock (writers here
 * free replaced nodes at once; the kernel frees them after a grace
 * period), the BPF memory allocator (this uses malloc()), and BTF.
 */
#ifndef LPM_H
#define LPM_H

#include <stddef.h>
#include <stdint.h>

#define LPM_DATA_MAX	16		/* enough for IPv6 */
#define LPM_FLAG_IM	1u		/* intermediate node: no value */

/* Update flags, as in the BPF UAPI. */
#ifndef BPF_ANY
#define BPF_ANY		0
#define BPF_NOEXIST	1
#define BPF_EXIST	2
#endif

struct lpm_key {
	uint32_t prefixlen;
	uint8_t data[LPM_DATA_MAX];	/* big-endian: data[0] is the top byte */
};

struct lpm_node {
	struct lpm_node *child[2];
	uint32_t prefixlen;
	uint32_t flags;
	uint32_t value;
	uint8_t data[LPM_DATA_MAX];
};

struct lpm_trie {
	struct lpm_node *root;
	size_t data_size;		/* bytes per key: 4 for IPv4 */
	uint32_t max_prefixlen;		/* 8 * data_size */
	size_t n_entries;		/* nodes with a value */
	size_t max_entries;
	size_t n_nodes;			/* all nodes, intermediate ones too */
};

void lpm_init(struct lpm_trie *t, size_t data_size, size_t max_entries);
void lpm_destroy(struct lpm_trie *t);
uint32_t *lpm_lookup(const struct lpm_trie *t, const struct lpm_key *key);
int lpm_update(struct lpm_trie *t, const struct lpm_key *key,
	       uint32_t value, int flags);
int lpm_delete(struct lpm_trie *t, const struct lpm_key *key);
int lpm_get_next_key(const struct lpm_trie *t, const struct lpm_key *key,
		     struct lpm_key *next);

/* Bit 'index' of a big-endian bit string. */
static inline int extract_bit(const uint8_t *data, uint32_t index)
{
	return !!(data[index / 8] & (1 << (7 - index % 8)));
}

#endif
