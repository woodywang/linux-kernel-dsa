// SPDX-License-Identifier: MIT
/*
 * mini_xa.c - a small teaching version of the XArray.
 *
 * A radix tree that maps unsigned long indices to entries.  Each node has
 * 64 slots, so each level consumes 6 bits of the index.  The tree is only
 * as tall as its largest index needs, growing and shrinking at the top.
 *
 * What it shows:              entry encoding in the low two bits (pointer,
 *                             value, internal), lookup, growing and
 *                             shrinking, three mark bitmaps per node that
 *                             are propagated upward, a search for marked
 *                             entries that skips unmarked subtrees, and
 *                             multi-index entries built from sibling slots.
 * What it leaves out on purpose: locking, RCU (and so retry entries),
 *                             allocation failure, the xa_state cursor,
 *                             splitting multi-index entries, and ID
 *                             allocation.
 *
 * Build and run the demo:     cc -O2 -Wall -o mini_xa mini_xa.c
 *                             ./mini_xa
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

_Static_assert(sizeof(unsigned long) == 8, "this code assumes 64-bit longs");

#define XA_CHUNK_SHIFT	6
#define XA_CHUNK_SIZE	(1UL << XA_CHUNK_SHIFT)	/* 64 slots per node */
#define XA_CHUNK_MASK	(XA_CHUNK_SIZE - 1)
#define XA_MAX_MARKS	3

struct xa_node {
	unsigned char shift;		/* index bits below this level */
	unsigned char offset;		/* our slot in the parent */
	unsigned char count;		/* non-NULL slots */
	struct xa_node *parent;		/* NULL for the top node */
	void *slots[XA_CHUNK_SIZE];
	unsigned long marks[XA_MAX_MARKS];	/* one bit per slot */
};

struct xarray {
	void *head;			/* NULL, the entry at index 0, or a node */
	unsigned int marks;		/* bit m: some entry has mark m */
	long nodes;			/* nodes currently allocated */
};

/* ------------------------------------------------------------------ */
/* Entries.  The bottom two bits say what an entry is:                 */
/*   00 pointer, x1 value (63-bit integer), 10 internal.               */

static inline void *xa_mk_value(unsigned long v)
{
	return (void *)(v << 1 | 1);
}

static inline unsigned long xa_to_value(const void *entry)
{
	return (unsigned long)entry >> 1;
}

static inline bool xa_is_value(const void *entry)
{
	return (unsigned long)entry & 1;
}

static inline void *xa_mk_internal(unsigned long v)
{
	return (void *)(v << 2 | 2);
}

static inline unsigned long xa_to_internal(const void *entry)
{
	return (unsigned long)entry >> 2;
}

static inline bool xa_is_internal(const void *entry)
{
	return ((unsigned long)entry & 3) == 2;
}

/* Internal entries 0-62 are siblings; 256 and 257 are reserved for the
 * kernel's retry and zero entries; node pointers are above 4096. */
#define XA_RETRY_ENTRY	xa_mk_internal(256)
#define XA_ZERO_ENTRY	xa_mk_internal(257)

static inline void *xa_mk_sibling(unsigned int offset)
{
	return xa_mk_internal(offset);
}

static inline bool xa_is_sibling(const void *entry)
{
	return xa_is_internal(entry) &&
		xa_to_internal(entry) < XA_CHUNK_SIZE - 1;
}

static inline unsigned int xa_to_sibling(const void *entry)
{
	return xa_to_internal(entry);
}

static inline void *xa_mk_node(const struct xa_node *node)
{
	return (void *)((unsigned long)node | 2);
}

static inline struct xa_node *xa_to_node(const void *entry)
{
	return (struct xa_node *)((unsigned long)entry - 2);
}

static inline bool xa_is_node(const void *entry)
{
	return xa_is_internal(entry) && (unsigned long)entry > 4096;
}

/* ------------------------------------------------------------------ */

#define XA_INIT		{ NULL, 0, 0 }

static unsigned int get_offset(unsigned long index, const struct xa_node *node)
{
	return (index >> node->shift) & XA_CHUNK_MASK;
}

/* The largest index the tree can hold without growing. */
static unsigned long max_index(const void *head)
{
	if (!xa_is_node(head))
		return 0;
	return (XA_CHUNK_SIZE << xa_to_node(head)->shift) - 1;
}

int xa_height(const struct xarray *xa)
{
	if (!xa_is_node(xa->head))
		return 0;
	return xa_to_node(xa->head)->shift / XA_CHUNK_SHIFT + 1;
}

static struct xa_node *node_new(struct xarray *xa, unsigned int shift,
				struct xa_node *parent, unsigned int offset)
{
	struct xa_node *node = calloc(1, sizeof(*node));

	if (!node)
		abort();		/* the kernel would return -ENOMEM */
	node->shift = shift;
	node->parent = parent;
	node->offset = offset;
	xa->nodes++;
	return node;
}

/* Free a node and everything below it. */
static void node_free(struct xarray *xa, struct xa_node *node)
{
	if (node->shift)
		for (unsigned int i = 0; i < XA_CHUNK_SIZE; i++)
			if (xa_is_node(node->slots[i]))
				node_free(xa, xa_to_node(node->slots[i]));
	free(node);
	xa->nodes--;
}

/*
 * Walk to the slot that holds index.  Returns the entry and sets *nodep
 * and *offp to its node and slot (*nodep is NULL for the head).  A
 * sibling slot leads to the first slot of its multi-index entry.
 */
static void *xa_walk(const struct xarray *xa, unsigned long index,
		     struct xa_node **nodep, unsigned int *offp)
{
	void *entry = xa->head;
	struct xa_node *node = NULL;
	unsigned int offset = 0;

	if (index > max_index(entry))
		entry = NULL;			/* beyond the tree's reach */
	while (xa_is_node(entry)) {
		node = xa_to_node(entry);
		offset = get_offset(index, node);
		entry = node->slots[offset];
		if (xa_is_sibling(entry)) {
			offset = xa_to_sibling(entry);
			entry = node->slots[offset];
		}
	}
	*nodep = node;
	*offp = offset;
	return entry;
}

void *xa_load(const struct xarray *xa, unsigned long index)
{
	struct xa_node *node;
	unsigned int offset;

	return xa_walk(xa, index, &node, &offset);
}

/* ------------------------------------------------------------------ */
/* Marks                                                               */

bool xa_get_mark(const struct xarray *xa, unsigned long index,
		 unsigned int mark)
{
	struct xa_node *node;
	unsigned int offset;

	if (!xa_walk(xa, index, &node, &offset))
		return false;
	if (!node)
		return xa->marks >> mark & 1;
	return node->marks[mark] >> offset & 1;
}

void xa_set_mark(struct xarray *xa, unsigned long index, unsigned int mark)
{
	struct xa_node *node;
	unsigned int offset;

	if (!xa_walk(xa, index, &node, &offset))
		return;				/* only entries can be marked */
	while (node) {
		if (node->marks[mark] & 1UL << offset)
			return;			/* the ancestors have it already */
		node->marks[mark] |= 1UL << offset;
		offset = node->offset;
		node = node->parent;
	}
	xa->marks |= 1U << mark;
}

/* Clear a mark bit, then the bit for each ancestor left with none. */
static void clear_up(struct xarray *xa, struct xa_node *node,
		     unsigned int offset, unsigned int mark)
{
	while (node) {
		node->marks[mark] &= ~(1UL << offset);
		if (node->marks[mark])
			return;			/* another slot still has it */
		offset = node->offset;
		node = node->parent;
	}
	xa->marks &= ~(1U << mark);
}

void xa_clear_mark(struct xarray *xa, unsigned long index, unsigned int mark)
{
	struct xa_node *node;
	unsigned int offset;

	if (xa_walk(xa, index, &node, &offset))
		clear_up(xa, node, offset, mark);
}

long xa_visits;				/* nodes looked at by searches */

/*
 * Find the first entry with the mark set whose first index is in
 * [*indexp, max].  Returns it and stores its index in *indexp, or
 * returns NULL.  Subtrees whose summary bit is clear are never entered.
 */
void *xa_find_marked(const struct xarray *xa, unsigned long *indexp,
		     unsigned long max, unsigned int mark)
{
	unsigned long index = *indexp;
	struct xa_node *node;
	unsigned int offset;

	if (!xa_is_node(xa->head)) {
		if (index || !(xa->marks >> mark & 1))
			return NULL;
		return xa->head;
	}
	if (index > max_index(xa->head))
		return NULL;
	node = xa_to_node(xa->head);
	offset = get_offset(index, node);
	xa_visits++;
	for (;;) {
		if (offset == XA_CHUNK_SIZE) {	/* done here: go up */
			if (!node->parent)
				return NULL;
			offset = node->offset + 1;
			node = node->parent;
			continue;
		}
		unsigned long bits = node->marks[mark] >> offset << offset;

		if (!bits) {
			offset = XA_CHUNK_SIZE;
			continue;
		}
		unsigned int next = __builtin_ctzl(bits);

		if (next != get_offset(index, node))	/* moved right */
			index = (index & ~((XA_CHUNK_SIZE << node->shift) - 1)) |
				(unsigned long)next << node->shift;
		if (index > max)
			return NULL;
		void *entry = node->slots[next];

		if (!xa_is_node(entry)) {
			if (index & ((1UL << node->shift) - 1)) {
				offset = next + 1;	/* began before *indexp */
				continue;
			}
			*indexp = index;
			return entry;
		}
		node = xa_to_node(entry);	/* descend */
		offset = get_offset(index, node);
		xa_visits++;
	}
}

/* ------------------------------------------------------------------ */
/* Growing and shrinking                                               */

/* Add levels at the top until a node with shift >= lshift covers max. */
static void xa_expand(struct xarray *xa, unsigned long max,
		      unsigned int lshift)
{
	unsigned int shift = lshift;

	if (!xa->head) {			/* empty: one node is enough */
		while (max >> shift > XA_CHUNK_MASK)
			shift += XA_CHUNK_SHIFT;
		xa->head = xa_mk_node(node_new(xa, shift, NULL, 0));
		return;
	}
	for (;;) {
		void *head = xa->head;
		struct xa_node *node;

		if (xa_is_node(head)) {
			struct xa_node *top = xa_to_node(head);

			if (max <= max_index(head) && top->shift >= lshift)
				return;
			shift = top->shift + XA_CHUNK_SHIFT;
		} else {
			shift = 0;		/* head held the entry at index 0 */
		}
		node = node_new(xa, shift, NULL, 0);
		node->slots[0] = head;		/* the old tree becomes slot 0 */
		node->count = 1;
		for (unsigned int m = 0; m < XA_MAX_MARKS; m++)
			if (xa->marks >> m & 1)
				node->marks[m] = 1;
		if (xa_is_node(head))
			xa_to_node(head)->parent = node;
		xa->head = xa_mk_node(node);
	}
}

void xa_destroy(struct xarray *xa)
{
	if (xa_is_node(xa->head))
		node_free(xa, xa_to_node(xa->head));
	xa->head = NULL;
	xa->marks = 0;
}

/* Remove top nodes that only lead to slot 0. */
static void xa_shrink(struct xarray *xa)
{
	while (xa_is_node(xa->head)) {
		struct xa_node *node = xa_to_node(xa->head);
		void *entry = node->slots[0];

		if (node->count != 1 || !entry)
			return;
		if (!xa_is_node(entry) && node->shift)
			return;			/* a multi-index entry stays put */
		xa->head = entry;
		if (xa_is_node(entry))
			xa_to_node(entry)->parent = NULL;
		free(node);
		xa->nodes--;
	}
}

/* ------------------------------------------------------------------ */
/* Storing and erasing                                                 */

/* Clear slots [first, end) of node, then delete empty nodes upward. */
static void node_erase(struct xarray *xa, struct xa_node *node,
		       unsigned int first, unsigned int end)
{
	for (unsigned int i = first; i < end; i++) {
		void *old = node->slots[i];

		if (!old)
			continue;
		for (unsigned int m = 0; m < XA_MAX_MARKS; m++)
			if (node->marks[m] >> i & 1)
				clear_up(xa, node, i, m);
		if (xa_is_node(old))
			node_free(xa, xa_to_node(old));
		node->slots[i] = NULL;
		node->count--;
	}
	while (!node->count) {			/* empty nodes go away */
		struct xa_node *parent = node->parent;

		if (parent) {
			parent->slots[node->offset] = NULL;
			parent->count--;
		} else {
			xa->head = NULL;
		}
		free(node);
		xa->nodes--;
		if (!parent)
			return;
		node = parent;
	}
	xa_shrink(xa);
}

/* Slots [offset, end) of one entry: the first slot and its siblings. */
static unsigned int entry_end(const struct xa_node *node, unsigned int offset)
{
	unsigned int end = offset + 1;

	while (end < XA_CHUNK_SIZE && node->slots[end] == xa_mk_sibling(offset))
		end++;
	return end;
}

/*
 * Store entry in slots [offset, offset + nr) of node, tying them together.
 * If the slots lie inside a larger entry, that whole entry is replaced.
 */
static void node_store(struct xarray *xa, struct xa_node *node,
		       unsigned int offset, unsigned int nr, void *entry)
{
	unsigned int first = offset, end = offset + nr;
	unsigned long squash[XA_MAX_MARKS] = { 0 };

	if (xa_is_sibling(node->slots[offset]))
		first = xa_to_sibling(node->slots[offset]);
	if (node->slots[first] && !xa_is_node(node->slots[first]) &&
	    (first < offset || entry_end(node, first) > end)) {
		if (entry)
			node->slots[first] = entry;	/* keeps its marks */
		else
			node_erase(xa, node, first, entry_end(node, first));
		return;
	}
	if (!entry) {
		node_erase(xa, node, offset, end);
		return;
	}
	for (unsigned int i = offset; i < end; i++) {
		void *old = node->slots[i];

		for (unsigned int m = 0; m < XA_MAX_MARKS; m++) {
			squash[m] |= node->marks[m] >> i & 1;
			node->marks[m] &= ~(1UL << i);
		}
		if (xa_is_node(old))
			node_free(xa, xa_to_node(old));
		node->count += !old;
		node->slots[i] = i == offset ? entry : xa_mk_sibling(offset);
	}
	for (unsigned int m = 0; m < XA_MAX_MARKS; m++)
		node->marks[m] |= squash[m] << offset;	/* marks move to slot 0 */
}

/*
 * Store entry at the 2^order indices starting at index, which must be a
 * multiple of 2^order.  Storing NULL erases.  Returns 0 or -EINVAL.
 */
int xa_store_order(struct xarray *xa, unsigned long index,
		   unsigned int order, void *entry)
{
	unsigned int lshift = order - order % XA_CHUNK_SHIFT;
	unsigned int nr = 1U << order % XA_CHUNK_SHIFT;
	unsigned long last;
	struct xa_node *node;

	if (order >= 64 || index & ((1UL << order) - 1) ||
	    xa_is_internal(entry))
		return -EINVAL;
	last = index + ((1UL << order) - 1);
	if (!xa_is_node(xa->head) && index == 0 && order == 0) {
		xa->head = entry;		/* no node needed */
		if (!entry)
			xa->marks = 0;
		return 0;
	}
	if (entry) {
		xa_expand(xa, last, lshift);
	} else if (index > max_index(xa->head)) {
		return 0;			/* nothing stored there */
	} else if (!xa_is_node(xa->head) ||
		   xa_to_node(xa->head)->shift < lshift) {
		xa_destroy(xa);			/* the range covers everything */
		return 0;
	}
	node = xa_to_node(xa->head);
	while (node->shift > lshift) {
		unsigned int offset = get_offset(index, node);
		void *next = node->slots[offset];

		if (xa_is_sibling(next))
			next = node->slots[offset = xa_to_sibling(next)];
		if (next && !xa_is_node(next)) {
			nr = 1;			/* inside a larger entry */
			break;
		}
		if (!next) {
			if (!entry)
				return 0;	/* nothing to erase */
			next = xa_mk_node(node_new(xa, node->shift -
						   XA_CHUNK_SHIFT, node, offset));
			node->slots[offset] = next;
			node->count++;
		}
		node = xa_to_node(next);
	}
	node_store(xa, node, get_offset(index, node), nr, entry);
	return 0;
}

int xa_store(struct xarray *xa, unsigned long index, void *entry)
{
	return xa_store_order(xa, index, 0, entry);
}

void xa_erase(struct xarray *xa, unsigned long index)
{
	xa_store_order(xa, index, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* Demo                                                                */

#ifndef MINI_XA_NO_MAIN

#define DIRTY		0		/* mark numbers, as in the page cache */
#define WRITEBACK	1

struct page {				/* a stand-in for a cached page */
	unsigned long index;
} __attribute__((aligned(8)));

static unsigned long rng = 88172645463325252UL;

static unsigned long xorshift(void)	/* deterministic on every libc */
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return rng;
}

static void show(const char *what, const struct xarray *xa)
{
	printf("%-26s height %d, %ld node%s\n", what, xa_height(xa), xa->nodes,
	       xa->nodes == 1 ? "" : "s");
}

int main(void)
{
	static struct page pages[1 << 18];
	struct xarray xa = XA_INIT;
	unsigned long i, n;

	for (i = 0; i < 1 << 18; i++)
		pages[i].index = i;

	/* 1. Height follows the largest index. */
	xa_store(&xa, 0, &pages[0]);
	show("store 0:", &xa);
	xa_store(&xa, 1, &pages[1]);
	show("store 1:", &xa);
	xa_store(&xa, 64, &pages[64]);
	show("store 64:", &xa);
	xa_store(&xa, 262143, &pages[262143]);
	show("store 262143:", &xa);
	xa_erase(&xa, 262143);
	show("erase 262143:", &xa);
	xa_erase(&xa, 64);
	xa_erase(&xa, 1);
	show("erase 64 and 1:", &xa);
	xa_destroy(&xa);

	/* 2. A 1 GiB file fully cached, three pages dirty. */
	for (i = 0; i < 1 << 18; i++)
		xa_store(&xa, i, &pages[i]);
	show("262144 pages:", &xa);
	xa_set_mark(&xa, 1000, DIRTY);
	xa_set_mark(&xa, 70000, DIRTY);
	xa_set_mark(&xa, 200000, DIRTY);
	printf("dirty pages:");
	xa_visits = 0;
	n = 0;
	for (i = 0; xa_find_marked(&xa, &i, ~0UL, DIRTY); i++, n++)
		printf(" %lu", i);
	printf(" (%lu found, %ld node visits)\n", n, xa_visits);

	/* 3. Evicted pages leave value entries ("shadows") behind. */
	for (i = 0; i < 64; i++)
		xa_store(&xa, i, xa_mk_value(1000 + i));
	void *e = xa_load(&xa, 5);

	printf("index 5: %s %lu; ", xa_is_value(e) ? "value" : "pointer",
	       xa_to_value(e));
	e = xa_load(&xa, 64);
	printf("index 64: %s to page %lu\n", xa_is_value(e) ? "value" :
	       "pointer", ((struct page *)e)->index);
	xa_destroy(&xa);

	/* 4. A 2 MiB folio: 512 indices as one entry or as 512 entries. */
	for (i = 512; i < 1024; i++)
		xa_store(&xa, i, &pages[512]);
	printf("512 single entries: %ld nodes; ", xa.nodes);
	xa_destroy(&xa);
	xa_store_order(&xa, 512, 9, &pages[512]);
	printf("one order-9 entry: %ld node\n", xa.nodes);
	xa_set_mark(&xa, 700, DIRTY);
	printf("load 700 -> page %lu, dirty at 512? %s\n",
	       ((struct page *)xa_load(&xa, 700))->index,
	       xa_get_mark(&xa, 512, DIRTY) ? "yes" : "no");
	xa_destroy(&xa);

	/* 5. Dense versus scattered indices. */
	for (i = 0; i < 4096; i++)
		xa_store(&xa, i, &pages[i]);
	printf("4096 dense:     %5ld nodes, %5.1f bytes per entry\n",
	       xa.nodes, (double)xa.nodes * sizeof(struct xa_node) / 4096);
	xa_destroy(&xa);
	for (n = 0; n < 4096; ) {
		i = xorshift() & 0xffffffffUL;	/* a 32-bit "hash" */
		if (!xa_load(&xa, i)) {
			xa_store(&xa, i, &pages[n]);
			n++;
		}
	}
	printf("4096 scattered: %5ld nodes, %5.1f bytes per entry\n",
	       xa.nodes, (double)xa.nodes * sizeof(struct xa_node) / 4096);
	xa_destroy(&xa);
	return 0;
}
#endif
