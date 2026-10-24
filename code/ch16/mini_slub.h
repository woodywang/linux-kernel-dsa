// SPDX-License-Identifier: MIT
/*
 * mini_slub.h - a small teaching version of the SLUB slab allocator as it
 * is in Linux v6.18: slabs with an in-object free list, a per-CPU active
 * slab with a "lockless" fast path guarded by a transaction id, per-CPU
 * and per-node partial lists, per-CPU sheaves with a barn, freelist
 * randomization, free-pointer hardening, and kmalloc size classes.
 *
 * What it leaves out on purpose:
 *   - Real concurrency.  "CPUs" are simulated: ms_cpu says which one we
 *     are running on, and the test switches it at random.  The kernel's
 *     atomic double-word compare-and-swap is emulated by a plain function
 *     (this_cpu_cmpxchg_double()), and a hook (ms_preempt_hook) lets a
 *     demo run code at the point where an interrupt could arrive.
 *   - NUMA: there is one memory node, so one partial list and one barn.
 *   - The kernel's page allocator: pages come from a fixed arena with a
 *     first-fit search (Chapter 15 covers the buddy allocator).
 *   - Debug features (red zones, owner tracking), memory cgroups,
 *     kfree_rcu() sheaves, bulk APIs, and cache merging.
 *
 * The struct and function names follow the kernel's where the idea is the
 * same, so that the real code in mm/slub.c is easy to find.
 */
#ifndef MINI_SLUB_H
#define MINI_SLUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ilist.h"

#define NR_CPUS			4
#define PAGE_SHIFT		12
#define PAGE_SIZE		(1UL << PAGE_SHIFT)
#define MAX_SLAB_ORDER		3	/* PAGE_ALLOC_COSTLY_ORDER */
#define MAX_FULL_SHEAVES	10
#define MAX_EMPTY_SHEAVES	10

/* Cache flags. */
#define MS_RANDOM	0x1	/* randomize the free list of new slabs */
#define MS_POISON	0x2	/* poison free objects and check on allocation */

struct kmem_cache;

/* The slab descriptor.  The kernel overlays it on the slab's struct page. */
struct slab {
	struct kmem_cache *cache;	/* NULL: not a slab */
	void *base;			/* address of the first object */
	void *freelist;			/* first free object, or NULL */
	unsigned int inuse;		/* objects not on @freelist */
	unsigned int objects;		/* objects in this slab */
	bool frozen;			/* the active slab of some CPU */
	bool on_partial;		/* on the node's partial list */
	struct list_head slab_list;	/* link in the node's partial list */
	struct slab *next;		/* link in a per-CPU partial list */
	unsigned int slabs;		/* per-CPU partial slabs from here on */
};

/* One per CPU per cache: the active slab and its private free list. */
struct kmem_cache_cpu {
	void *freelist;			/* next free object of @slab */
	unsigned long tid;		/* transaction id, bumped by every change */
	struct slab *slab;		/* the slab we allocate from */
	struct slab *partial;		/* partially free slabs kept for later */
};

/* A sheaf: an array of free objects (a "magazine" in Bonwick's terms). */
struct slab_sheaf {
	struct list_head barn_list;
	unsigned int size;		/* objects[0..size) are free objects */
	void *objects[];
};

struct slub_percpu_sheaves {
	bool locked;			/* the kernel's local_trylock_t */
	struct slab_sheaf *main;	/* never NULL */
	struct slab_sheaf *spare;	/* empty, full, or NULL */
};

/* The per-node stock of full and empty sheaves. */
struct node_barn {
	struct list_head sheaves_full;
	struct list_head sheaves_empty;
	unsigned int nr_full;
	unsigned int nr_empty;
};

struct ms_stats {
	unsigned long alloc_fast, alloc_slow, alloc_pcs;
	unsigned long free_fast, free_slow, free_pcs;
	unsigned long cmpxchg_fail, barn_get, barn_put;
	unsigned long sheaf_refill, sheaf_flush;
	unsigned long new_slabs, freed_slabs;
};

struct kmem_cache {
	const char *name;
	unsigned int object_size;	/* what the user asked for */
	unsigned int size;		/* object plus metadata, aligned */
	unsigned int align;
	unsigned int offset;		/* where the free pointer lives */
	unsigned int order;		/* a slab is 2^order pages */
	unsigned int objects;		/* objects per slab */
	unsigned int flags;
	uintptr_t random;		/* secret for free pointer encoding */
	unsigned int *random_seq;	/* shuffled object offsets, or NULL */
	unsigned int min_partial;	/* keep this many slabs, even if empty */
	unsigned int cpu_partial_slabs;	/* per-CPU partial list limit */
	unsigned int sheaf_capacity;	/* 0: no sheaves */
	struct kmem_cache_cpu cpu_slab[NR_CPUS];
	struct slub_percpu_sheaves cpu_sheaves[NR_CPUS];
	struct list_head partial;	/* the node's partial slabs */
	unsigned long nr_partial;
	struct node_barn barn;
	struct ms_stats stat;
};

/* The simulated CPU we are running on; tests and demos change it. */
extern int ms_cpu;
/* Called where an interrupt or a migration could hit a fast path. */
extern void (*ms_preempt_hook)(void);
/* Nonzero while a per-CPU lock is held: no migration allowed then. */
extern int ms_preempt_count;

/* Set up the page arena and the kmalloc caches; seed the RNG. */
void ms_init(unsigned long nr_pages, uint64_t seed);
void ms_exit(void);
unsigned long ms_pages_in_use(void);
struct slab *virt_to_slab(const void *p);

struct kmem_cache *ms_cache_create(const char *name, unsigned int size,
				   unsigned int align, unsigned int flags,
				   unsigned int sheaf_capacity);
void ms_cache_destroy(struct kmem_cache *s);
void *ms_cache_alloc(struct kmem_cache *s);
void ms_cache_free(struct kmem_cache *s, void *object);
void ms_cache_shrink(struct kmem_cache *s);

/* Prefilled sheaves: guaranteed allocations for a short operation. */
struct slab_sheaf *ms_prefill_sheaf(struct kmem_cache *s, unsigned int count);
void *ms_alloc_from_sheaf(struct kmem_cache *s, struct slab_sheaf *sheaf);
void ms_return_sheaf(struct kmem_cache *s, struct slab_sheaf *sheaf);

/* kmalloc() on top of power-of-two (and 96, 192) size classes. */
#define KMALLOC_SHIFT_HIGH	(PAGE_SHIFT + 1)
#define KMALLOC_MAX_CACHE_SIZE	(1UL << KMALLOC_SHIFT_HIGH)
unsigned int kmalloc_index(size_t size);
struct kmem_cache *kmalloc_cache(unsigned int index);
void *ms_kmalloc(size_t size);
void ms_kfree(const void *p);

/* For tests: decode a free pointer, and the RNG. */
void *get_freepointer(const struct kmem_cache *s, void *object);
uint32_t ms_random_u32(void);
/* For the demo: make the fast path compare the free list pointer only. */
void ms_debug_disable_tid(bool off);

#endif
