// SPDX-License-Identifier: MIT
/*
 * htab.h - a preallocated hash map in the shape of the BPF hash map
 * (BPF_MAP_TYPE_HASH without BPF_F_NO_PREALLOC), for Chapter 20.
 *
 * All max_entries elements, plus one spare per CPU, are allocated when the
 * map is created; updates and deletes never call malloc() or free().  Free
 * elements sit on per-CPU free lists.  Buckets are singly linked chains
 * whose end marker records the bucket number, as with hlist_nulls.
 *
 * What it leaves out on purpose: real concurrency (CPUs are simulated by
 * the variable htab_cpu, and there are no locks), per-CPU values, LRU
 * eviction, BTF special fields, batch operations and iteration.
 */
#ifndef HTAB_H
#define HTAB_H

#include <stddef.h>
#include <stdint.h>

#define NR_CPUS		4

/* Update flags, as in the BPF UAPI. */
#ifndef BPF_ANY
#define BPF_ANY		0	/* create or update */
#define BPF_NOEXIST	1	/* create only */
#define BPF_EXIST	2	/* update only */
#endif

struct htab_elem {
	struct htab_elem *next;		/* hash chain; a nulls marker at the end */
	struct htab_elem *free_next;	/* free list (the kernel overlays pprev) */
	uint32_t hash;
	uint8_t key[] __attribute__((aligned(8)));	/* key, then value */
};

struct bucket {
	struct htab_elem *first;	/* the kernel adds a lock here */
};

struct htab {
	uint32_t key_size, value_size, max_entries;
	uint32_t n_buckets;		/* a power of two >= max_entries */
	uint32_t elem_size;
	uint32_t hashrnd;		/* random seed in the kernel */
	struct bucket *buckets;
	char *elems;			/* max_entries + NR_CPUS elements */
	struct htab_elem *freelist[NR_CPUS];
	struct htab_elem *extra[NR_CPUS];	/* one spare element per CPU */
};

extern int htab_cpu;			/* the CPU the caller "runs" on */
extern unsigned long htab_nr_mallocs;	/* calls to malloc() so far */

struct htab *htab_create(uint32_t key_size, uint32_t value_size,
			 uint32_t max_entries, uint32_t seed);
void htab_destroy(struct htab *h);
void *htab_lookup(struct htab *h, const void *key);
int htab_update(struct htab *h, const void *key, const void *value,
		int flags);
int htab_delete(struct htab *h, const void *key);

/* For tests and the demo. */
struct htab_elem *htab_elem_at(const struct htab *h, uint32_t i);
int htab_free_count(const struct htab *h, int cpu);

/* Nulls markers: odd "pointers" that carry a bucket number. */
static inline struct htab_elem *nulls_marker(uint32_t bucket)
{
	return (struct htab_elem *)(((uintptr_t)bucket << 1) | 1);
}

static inline int is_nulls(const struct htab_elem *p)
{
	return (uintptr_t)p & 1;
}

static inline uint32_t nulls_value(const struct htab_elem *p)
{
	return (uintptr_t)p >> 1;
}

#endif
