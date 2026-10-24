// SPDX-License-Identifier: MIT
/*
 * slub_demo.c - small experiments with mini_slub.c whose output the
 * chapter quotes: cache layouts, kmalloc size classes, what the
 * transaction id protects against, sheaves with a producer on one CPU and
 * a consumer on another, and freelist randomization.
 *
 *   cc -O2 -Wall -o slub_demo slub_demo.c mini_slub.c && ./slub_demo
 */
#include <stdio.h>
#include <string.h>
#include "mini_slub.h"

static unsigned int idx(struct kmem_cache *s, const void *p)
{
	struct slab *slab = virt_to_slab(p);

	return (unsigned int)(((const char *)p - (char *)slab->base) / s->size);
}

static void layouts(void)
{
	struct kmem_cache *c[] = {
		kmalloc_cache(kmalloc_index(8)),
		kmalloc_cache(kmalloc_index(96)),
		kmalloc_cache(kmalloc_index(700)),
		kmalloc_cache(kmalloc_index(8192)),
		ms_cache_create("obj-200/64", 200, 64, 0, 0),
		ms_cache_create("obj-24+poison", 24, 0, MS_POISON, 0),
	};

	printf("== Cache layouts ==\n");
	printf("%-14s %6s %5s %8s %7s %5s %11s %11s\n", "cache", "object",
	       "size", "freeptr", "objects", "order", "min_partial",
	       "cpu_partial");
	for (unsigned int i = 0; i < sizeof(c) / sizeof(c[0]); i++)
		printf("%-14s %6u %5u %8u %7u %5u %11u %11u\n", c[i]->name,
		       c[i]->object_size, c[i]->size, c[i]->offset,
		       c[i]->objects, c[i]->order, c[i]->min_partial,
		       c[i]->cpu_partial_slabs);
	ms_cache_destroy(c[4]);
	ms_cache_destroy(c[5]);
}

static void size_classes(void)
{
	static const size_t sizes[] = { 1, 8, 9, 24, 64, 65, 96, 97, 129,
					193, 1000, 4097, 8192, 8193 };

	printf("\n== kmalloc size classes ==\n");
	for (unsigned int i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		size_t n = sizes[i];

		if (n > KMALLOC_MAX_CACHE_SIZE)
			printf("%5zu -> pages\n", n);
		else
			printf("%5zu -> %s\n", n,
			       kmalloc_cache(kmalloc_index(n))->name);
	}
}

/* ---- The transaction id ---- */

static struct kmem_cache *tc;
static void *held[2];

/* An "interrupt" that takes the next two objects and frees the first. */
static void interrupt(void)
{
	ms_preempt_hook = NULL;
	held[0] = ms_cache_alloc(tc);
	held[1] = ms_cache_alloc(tc);
	ms_cache_free(tc, held[0]);
}

static void tid_run(bool with_tid)
{
	void *first, *got, *next1, *next2;

	tc = ms_cache_create("obj-64", 64, 0, 0, 0);
	ms_debug_disable_tid(!with_tid);
	ms_cpu = 0;
	first = ms_cache_alloc(tc);		/* object 0 starts the slab */
	ms_preempt_hook = interrupt;
	got = ms_cache_alloc(tc);
	next1 = ms_cache_alloc(tc);
	next2 = ms_cache_alloc(tc);
	printf("  %s tid: %lu cmpxchg failure(s); got object %u; "
	       "interrupt holds %u; next allocations: %u %u%s\n",
	       with_tid ? "with   " : "without", tc->stat.cmpxchg_fail,
	       idx(tc, got), idx(tc, held[1]), idx(tc, next1), idx(tc, next2),
	       next1 == held[1] ? "  <- object 2 handed out twice" : "");
	/* Leave the broken cache alone; just put the pages back. */
	(void)first;
	ms_debug_disable_tid(false);
}

static void migrate(void)
{
	ms_preempt_hook = NULL;
	printf("  (migrated to CPU 1; its tid is %lu)\n",
	       tc->cpu_slab[1].tid);
	ms_cpu = 1;
}

static void tid_demo(void)
{
	void *a, *b;

	printf("\n== The transaction id ==\n");
	printf("interrupt between reading the free list and the cmpxchg:\n"
	       "it allocates objects 1 and 2, then frees object 1\n");
	tid_run(true);
	tid_run(false);

	printf("migration between reading the free list and the cmpxchg:\n");
	tc = ms_cache_create("obj-64", 64, 0, 0, 0);
	ms_cpu = 1;
	b = ms_cache_alloc(tc);			/* CPU 1 gets a slab of its own */
	ms_cpu = 0;
	a = ms_cache_alloc(tc);
	printf("  read on CPU 0 with tid %lu\n", tc->cpu_slab[0].tid);
	ms_preempt_hook = migrate;
	a = ms_cache_alloc(tc);
	printf("  %lu cmpxchg failure(s); object came from CPU %d's slab "
	       "(same slab as CPU 1's first object: %s)\n",
	       tc->stat.cmpxchg_fail, ms_cpu,
	       virt_to_slab(a) == virt_to_slab(b) ? "yes" : "no");
	ms_cpu = 0;
}

/* ---- Producer on one CPU, consumer on another ---- */

static void producer_consumer(unsigned int sheaf_capacity)
{
	struct kmem_cache *s = ms_cache_create("obj-64", 64, 0, 0,
					       sheaf_capacity);
	static void *objs[100];
	struct ms_stats *st = &s->stat;

	for (int round = 0; round < 100; round++) {
		ms_cpu = 0;
		for (int i = 0; i < 100; i++)
			objs[i] = ms_cache_alloc(s);
		ms_cpu = 1;
		for (int i = 0; i < 100; i++)
			ms_cache_free(s, objs[i]);
	}
	if (!sheaf_capacity)
		printf("  no sheaves:  alloc fast %5lu slow %4lu | free fast %5lu "
		       "slow %5lu | slabs %lu\n", st->alloc_fast, st->alloc_slow,
		       st->free_fast, st->free_slow, st->new_slabs);
	else
		printf("  sheaves(%u): alloc sheaf %5lu, barn gives %3lu full | "
		       "free sheaf %5lu, barn takes %3lu full | slabs %lu\n",
		       sheaf_capacity, st->alloc_pcs, st->barn_get,
		       st->free_pcs, st->barn_put, st->new_slabs);
	ms_cache_destroy(s);
}

/* ---- Freelist randomization ---- */

static void freelist_order(unsigned int flags)
{
	struct kmem_cache *s = ms_cache_create("obj-128", 128, 0, flags, 0);
	void *p[64];

	printf("  %s:", flags ? "randomized" : "sequential");
	for (int slab = 0; slab < 2; slab++) {
		printf(slab ? "  | slab 2:" : " slab 1:");
		for (int i = 0; i < 8; i++) {
			p[slab * 32 + i] = ms_cache_alloc(s);
			printf(" %2u", idx(s, p[slab * 32 + i]));
		}
		for (int i = 8; i < 32; i++)	/* use up the slab */
			p[slab * 32 + i] = ms_cache_alloc(s);
	}
	printf("\n");
	if (flags) {
		printf("  the cache's shuffled order:");
		for (unsigned int i = 0; i < 16; i++)
			printf(" %u", s->random_seq[i] / s->size);
		printf(" ...\n");
	}
	for (int i = 0; i < 64; i++)
		ms_cache_free(s, p[i]);
	ms_cache_destroy(s);
}

static void hardening(void)
{
	struct kmem_cache *s = ms_cache_create("obj-128", 128, 0, 0, 0);
	void *p = ms_cache_alloc(s);
	struct slab *slab = virt_to_slab(p);
	unsigned int raw_inside = 0, links = 0;

	for (void *q = s->cpu_slab[0].freelist; q; q = get_freepointer(s, q)) {
		char *raw = *(char **)((char *)q + s->offset);

		links++;
		raw_inside += raw >= (char *)slab->base &&
			      raw < (char *)slab->base + PAGE_SIZE;
	}
	printf("\n== Free pointer hardening ==\n");
	printf("  %u free objects; stored links that point into the slab: %u\n",
	       links, raw_inside);
	ms_cache_free(s, p);
	ms_cache_destroy(s);
}

int main(void)
{
	ms_init(4096, 2026);
	layouts();
	size_classes();
	tid_demo();
	printf("\n== Producer on CPU 0, consumer on CPU 1 "
	       "(100 rounds of 100 objects) ==\n");
	producer_consumer(0);
	producer_consumer(32);
	ms_cpu = 0;
	printf("\n== Freelist order (first 8 allocations from each new slab) ==\n");
	freelist_order(0);
	freelist_order(MS_RANDOM);
	hardening();
	return 0;
}
