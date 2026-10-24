// SPDX-License-Identifier: MIT
/*
 * mini_slub.c - a small teaching version of the SLUB allocator (v6.18).
 * See mini_slub.h for what it covers and what it leaves out.
 *
 * Layers, from the caller down:
 *   per-CPU sheaves (arrays of free objects)      alloc_from_pcs(), free_to_pcs()
 *   the barn (a stock of full and empty sheaves)  barn_*()
 *   the per-CPU active slab and its free list     slab_alloc(), do_slab_free()
 *   per-CPU partial slabs                         put_cpu_partial()
 *   the node's partial list                       get_partial(), __slab_free()
 *   pages                                         alloc_pages(), free_pages()
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mini_slub.h"

#define ALIGN(x, a)		(((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a)	((x) & ~((a) - 1))
#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#define POISON_FREE		0x6b	/* the kernel's value, too */

#if UINTPTR_MAX > 0xffffffffu
#define swab(x)		__builtin_bswap64(x)
#else
#define swab(x)		__builtin_bswap32(x)
#endif

#define swap(a, b) do { typeof(a) _t = (a); (a) = (b); (b) = _t; } while (0)

int ms_cpu;
void (*ms_preempt_hook)(void);
int ms_preempt_count;

static void preempt_point(void)
{
	if (ms_preempt_hook)
		ms_preempt_hook();
}

static void ms_bug(const struct kmem_cache *s, const char *what)
{
	fprintf(stderr, "mini_slub: %s: %s\n", s ? s->name : "?", what);
	abort();
}

static unsigned int fls(unsigned long x)	/* 1-based last set bit */
{
	return x ? 8 * sizeof(x) - __builtin_clzl(x) : 0;
}

static unsigned int ilog2(unsigned long x)
{
	return fls(x) - 1;
}

static unsigned int get_order(unsigned long size)	/* pages, rounded up */
{
	unsigned int order = 0;

	while ((PAGE_SIZE << order) < size)
		order++;
	return order;
}

/* ------------------------------------------------------------------ */
/* A deterministic RNG (splitmix64), so that runs can be repeated.    */

static uint64_t rng_state;

uint32_t ms_random_u32(void)
{
	uint64_t z = (rng_state += 0x9e3779b97f4a7c15ULL);

	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return (uint32_t)((z ^ (z >> 31)) >> 32);
}

/* A number in [0, ceil), like the kernel's get_random_u32_below() but
 * without its rejection step, so very slightly biased. */
static uint32_t random_u32_below(uint32_t ceil)
{
	return (uint32_t)(((uint64_t)ms_random_u32() * ceil) >> 32);
}

/* ------------------------------------------------------------------ */
/* The page source: a fixed arena and one descriptor per page.         */

struct page {
	struct page *head;		/* first page of the block */
	unsigned int order;		/* head only: block is 2^order pages */
	bool used;
	struct slab slab;		/* head only, when the block is a slab */
};

static char *arena;
static struct page *pages;
static unsigned long nr_pages, pages_in_use, next_fit;

static struct page *virt_to_page(const void *p)
{
	unsigned long pfn = ((const char *)p - arena) >> PAGE_SHIFT;

	if ((const char *)p < arena || pfn >= nr_pages)
		return NULL;
	return &pages[pfn];
}

/* Find 2^order free, naturally aligned pages (next fit, wrapping once). */
static void *alloc_pages(unsigned int order)
{
	unsigned long n = 1UL << order;

	for (unsigned long tries = 0; tries < nr_pages; tries += n) {
		unsigned long pfn = ALIGN(next_fit, n) % nr_pages, i;

		for (i = 0; i < n && !pages[pfn + i].used; i++)
			;
		next_fit = pfn + n;
		if (i < n)
			continue;
		for (i = 0; i < n; i++) {
			pages[pfn + i].used = true;
			pages[pfn + i].head = &pages[pfn];
		}
		pages[pfn].order = order;
		pages_in_use += n;
		return arena + (pfn << PAGE_SHIFT);
	}
	return NULL;
}

static void free_pages(void *addr, unsigned int order)
{
	struct page *pg = virt_to_page(addr);

	for (unsigned long i = 0; i < (1UL << order); i++) {
		pg[i].used = false;
		pg[i].head = NULL;
	}
	memset(&pg->slab, 0, sizeof(pg->slab));
	pages_in_use -= 1UL << order;
}

/* Every object knows its slab: the descriptor sits with the page. */
struct slab *virt_to_slab(const void *p)
{
	struct page *pg = virt_to_page(p);

	if (!pg || !pg->used || !pg->head->slab.cache)
		return NULL;
	return &pg->head->slab;
}

unsigned long ms_pages_in_use(void)
{
	return pages_in_use;
}

/* ------------------------------------------------------------------ */
/* Free pointers.                                                      */

/*
 * The link to the next free object is stored inside the free object, at
 * s->offset.  It is stored encoded, as CONFIG_SLAB_FREELIST_HARDENED
 * does: XOR with a per-cache secret and the byte-swapped address of the
 * slot itself.
 */
void *get_freepointer(const struct kmem_cache *s, void *object)
{
	uintptr_t addr = (uintptr_t)object + s->offset;
	uintptr_t stored = *(uintptr_t *)addr;

	return (void *)(stored ^ s->random ^ swab(addr));
}

static void set_freepointer(const struct kmem_cache *s, void *object,
			    void *fp)
{
	uintptr_t addr = (uintptr_t)object + s->offset;

	if (object == fp)		/* naive detection of a double free */
		ms_bug(s, "double free or corrupted free list");
	*(uintptr_t *)addr = (uintptr_t)fp ^ s->random ^ swab(addr);
}

/* ------------------------------------------------------------------ */
/* Freelist randomization.                                             */

/* One Fisher-Yates shuffle per cache, of object offsets 0..objects-1. */
static void freelist_randomize(unsigned int *list, unsigned int count)
{
	for (unsigned int i = 0; i < count; i++)
		list[i] = i;
	for (unsigned int i = count - 1; i > 0; i--) {
		unsigned int r = random_u32_below(i + 1);

		swap(list[i], list[r]);
	}
}

/* Link a new slab's objects in the cache's shuffled order, starting at a
 * random point of that order. */
static bool shuffle_freelist(struct kmem_cache *s, struct slab *slab)
{
	unsigned int pos, count = slab->objects;
	char *start = slab->base;
	void *cur, *next;

	if (count < 2 || !s->random_seq)
		return false;
	pos = random_u32_below(count);
	cur = start + s->random_seq[pos];
	slab->freelist = cur;
	for (unsigned int i = 1; i < count; i++) {
		pos = (pos + 1) % count;
		next = start + s->random_seq[pos];
		set_freepointer(s, cur, next);
		cur = next;
	}
	set_freepointer(s, cur, NULL);
	return true;
}

/* ------------------------------------------------------------------ */
/* Slabs.                                                              */

static void poison_object(const struct kmem_cache *s, void *object)
{
	if (s->flags & MS_POISON)
		memset(object, POISON_FREE, s->object_size);
}

static struct slab *new_slab(struct kmem_cache *s)
{
	char *base = alloc_pages(s->order);
	struct slab *slab;

	if (!base)
		return NULL;
	slab = &virt_to_page(base)->slab;
	*slab = (struct slab){ .cache = s, .base = base,
			       .objects = s->objects };
	list_init(&slab->slab_list);
	for (unsigned int i = 0; i < s->objects; i++)
		poison_object(s, base + i * s->size);
	if (!shuffle_freelist(s, slab)) {
		slab->freelist = base;
		for (unsigned int i = 0; i < s->objects; i++) {
			char *obj = base + i * s->size;

			set_freepointer(s, obj, i + 1 < s->objects ?
					obj + s->size : NULL);
		}
	}
	s->stat.new_slabs++;
	return slab;
}

static void discard_slab(struct kmem_cache *s, struct slab *slab)
{
	s->stat.freed_slabs++;
	free_pages(slab->base, s->order);
}

static void add_partial(struct kmem_cache *s, struct slab *slab)
{
	list_add_tail(&slab->slab_list, &s->partial);
	slab->on_partial = true;
	s->nr_partial++;
}

static void remove_partial(struct kmem_cache *s, struct slab *slab)
{
	list_del(&slab->slab_list);
	slab->on_partial = false;
	s->nr_partial--;
}

/* ------------------------------------------------------------------ */
/* The per-CPU slab.                                                   */

/*
 * Transaction ids start at the CPU number and step by a power of two at
 * least NR_CPUS, so two CPUs never use the same tid.
 */
#define TID_STEP	4		/* roundup_pow_of_two(NR_CPUS) */
static unsigned long tid_step = TID_STEP;

static unsigned long next_tid(unsigned long tid)
{
	return tid + tid_step;
}

void ms_debug_disable_tid(bool off)
{
	tid_step = off ? 0 : TID_STEP;
}

static struct kmem_cache_cpu *this_cpu_ptr(struct kmem_cache *s)
{
	return &s->cpu_slab[ms_cpu];
}

/*
 * The kernel does this with one double-word compare-and-swap on the
 * (freelist, tid) pair of the CPU it is running on *now*; no lock, and
 * no atomic bus lock either, because only this CPU writes the pair.
 */
static bool this_cpu_cmpxchg_double(struct kmem_cache *s,
				    void *old_list, unsigned long old_tid,
				    void *new_list, unsigned long new_tid)
{
	struct kmem_cache_cpu *c = this_cpu_ptr(s);

	if (c->freelist != old_list || c->tid != old_tid)
		return false;
	c->freelist = new_list;
	c->tid = new_tid;
	return true;
}

/* Take all of @slab's free objects; the CPU keeps the slab (frozen) only
 * if there was something to take. */
static void *get_freelist(struct slab *slab)
{
	void *freelist = slab->freelist;

	slab->freelist = NULL;
	slab->inuse = slab->objects;
	slab->frozen = freelist != NULL;
	return freelist;
}

/* Move every slab on a per-CPU partial chain to the node's list. */
static void put_partials(struct kmem_cache *s, struct slab *slab)
{
	while (slab) {
		struct slab *next = slab->next;

		slab->next = NULL;
		if (!slab->inuse && s->nr_partial >= s->min_partial)
			discard_slab(s, slab);
		else
			add_partial(s, slab);
		slab = next;
	}
}

static void put_cpu_partial(struct kmem_cache *s, struct slab *slab,
			    bool drain)
{
	struct kmem_cache_cpu *c = this_cpu_ptr(s);
	struct slab *oldslab = c->partial, *slab_to_put = NULL;
	unsigned int slabs = 0;

	if (oldslab) {
		if (drain && oldslab->slabs >= s->cpu_partial_slabs) {
			slab_to_put = oldslab;	/* full: move them all */
			oldslab = NULL;
		} else {
			slabs = oldslab->slabs;
		}
	}
	slab->slabs = slabs + 1;
	slab->next = oldslab;
	c->partial = slab;
	if (slab_to_put)
		put_partials(s, slab_to_put);
}

/* Take one slab from the node's list for the CPU, and up to half the
 * per-CPU partial limit more for later. */
static struct slab *get_partial(struct kmem_cache *s)
{
	struct slab *slab, *tmp, *first = NULL;
	unsigned int taken = 0;

	list_for_each_entry_safe(slab, tmp, &s->partial, slab_list) {
		remove_partial(s, slab);
		if (!first) {
			first = slab;
			if (!s->cpu_partial_slabs)
				break;
		} else {
			put_cpu_partial(s, slab, false);
			if (++taken > s->cpu_partial_slabs / 2)
				break;
		}
	}
	return first;
}

/* Give the CPU's slab back: merge its private list into the slab's. */
static void deactivate_slab(struct kmem_cache *s, struct slab *slab,
			    void *freelist)
{
	while (freelist) {
		void *next = get_freepointer(s, freelist);

		set_freepointer(s, freelist, slab->freelist);
		slab->freelist = freelist;
		slab->inuse--;
		freelist = next;
	}
	slab->frozen = false;
	if (!slab->inuse && s->nr_partial >= s->min_partial)
		discard_slab(s, slab);
	else if (slab->freelist)
		add_partial(s, slab);
	/* else: full, and full slabs are on no list */
}

static void *slab_alloc_slow(struct kmem_cache *s)
{
	struct kmem_cache_cpu *c = this_cpu_ptr(s);
	struct slab *slab = c->slab;
	void *freelist;

	s->stat.alloc_slow++;
	if (slab) {
		/* 1. Objects other CPUs freed wait on the slab's own list. */
		freelist = get_freelist(slab);
		if (freelist)
			goto load_freelist;
		c->slab = NULL;		/* full: it leaves the CPU */
	}
	if (c->partial) {
		/* 2. A slab from this CPU's partial list. */
		slab = c->partial;
		c->partial = slab->next;
	} else {
		/* 3. A slab from the node, or 4. a new one. */
		slab = get_partial(s);
		if (!slab)
			slab = new_slab(s);
		if (!slab)
			return NULL;
	}
	freelist = get_freelist(slab);
	c->slab = slab;
load_freelist:
	c->freelist = get_freepointer(s, freelist);
	c->tid = next_tid(c->tid);
	return freelist;
}

static void *slab_alloc(struct kmem_cache *s)
{
	struct kmem_cache_cpu *c;
	unsigned long tid;
	void *object, *next;

redo:
	c = this_cpu_ptr(s);
	tid = c->tid;
	object = c->freelist;
	if (!object || !c->slab)
		return slab_alloc_slow(s);
	next = get_freepointer(s, object);
	preempt_point();		/* an interrupt or migration may hit here */
	if (!this_cpu_cmpxchg_double(s, object, tid, next, next_tid(tid))) {
		s->stat.cmpxchg_fail++;
		goto redo;
	}
	s->stat.alloc_fast++;
	return object;
}

/* Free to a slab that is not this CPU's active slab. */
static void __slab_free(struct kmem_cache *s, struct slab *slab,
			void *object)
{
	void *prior = slab->freelist;
	bool was_frozen = slab->frozen;

	s->stat.free_slow++;
	/* The kernel updates (freelist, inuse) with one cmpxchg on the slab. */
	set_freepointer(s, object, prior);
	slab->freelist = object;
	slab->inuse--;

	if (was_frozen)
		return;			/* some CPU owns it: no list work */
	if (!prior) {
		put_cpu_partial(s, slab, true);	/* was full, on no list */
		return;
	}
	if (!slab->on_partial)
		return;			/* on some CPU's partial list */
	if (!slab->inuse && s->nr_partial >= s->min_partial) {
		remove_partial(s, slab);
		discard_slab(s, slab);
	}
}

static void do_slab_free(struct kmem_cache *s, struct slab *slab,
			 void *object)
{
	struct kmem_cache_cpu *c;
	unsigned long tid;
	void *freelist;

redo:
	c = this_cpu_ptr(s);
	tid = c->tid;
	if (slab != c->slab) {
		__slab_free(s, slab, object);
		return;
	}
	freelist = c->freelist;
	set_freepointer(s, object, freelist);
	preempt_point();
	if (!this_cpu_cmpxchg_double(s, freelist, tid, object, next_tid(tid))) {
		s->stat.cmpxchg_fail++;
		goto redo;
	}
	s->stat.free_fast++;
}

/* ------------------------------------------------------------------ */
/* Sheaves and the barn.                                               */

static struct slub_percpu_sheaves *this_cpu_sheaves(struct kmem_cache *s)
{
	return &s->cpu_sheaves[ms_cpu];
}

/*
 * The kernel protects a CPU's sheaves with a local lock that an interrupt
 * on the same CPU can only *try* to take; if it fails, the interrupt uses
 * the slab layer instead.  Holding it also prevents migration.
 */
static bool local_trylock(struct slub_percpu_sheaves *pcs)
{
	if (pcs->locked)
		return false;
	pcs->locked = true;
	ms_preempt_count++;
	return true;
}

static void local_unlock(struct slub_percpu_sheaves *pcs)
{
	pcs->locked = false;
	ms_preempt_count--;
}

static struct slab_sheaf *alloc_empty_sheaf(struct kmem_cache *s)
{
	struct slab_sheaf *sheaf = calloc(1, sizeof(*sheaf) +
					  s->sheaf_capacity * sizeof(void *));

	if (sheaf)
		list_init(&sheaf->barn_list);
	return sheaf;
}

static void free_empty_sheaf(struct slab_sheaf *sheaf)
{
	free(sheaf);
}

/* Fill a sheaf from the slab layer. */
static int refill_sheaf(struct kmem_cache *s, struct slab_sheaf *sheaf)
{
	while (sheaf->size < s->sheaf_capacity) {
		void *object = slab_alloc(s);

		if (!object)
			return -1;
		sheaf->objects[sheaf->size++] = object;
		s->stat.sheaf_refill++;
	}
	return 0;
}

/* Return a sheaf's objects to their slabs. */
static void sheaf_flush(struct kmem_cache *s, struct slab_sheaf *sheaf)
{
	for (unsigned int i = 0; i < sheaf->size; i++) {
		void *object = sheaf->objects[i];

		do_slab_free(s, virt_to_slab(object), object);
	}
	s->stat.sheaf_flush += sheaf->size;
	sheaf->size = 0;
}

static struct slab_sheaf *barn_take(struct list_head *list,
				    unsigned int *nr)
{
	struct slab_sheaf *sheaf;

	if (!*nr)
		return NULL;
	sheaf = list_entry(list->next, struct slab_sheaf, barn_list);
	list_del(&sheaf->barn_list);
	(*nr)--;
	return sheaf;
}

static struct slab_sheaf *barn_get_empty_sheaf(struct node_barn *barn)
{
	return barn_take(&barn->sheaves_empty, &barn->nr_empty);
}

static void barn_put_empty_sheaf(struct node_barn *barn,
				 struct slab_sheaf *sheaf)
{
	list_add(&sheaf->barn_list, &barn->sheaves_empty);
	barn->nr_empty++;
}

static void barn_put_full_sheaf(struct node_barn *barn,
				struct slab_sheaf *sheaf)
{
	list_add(&sheaf->barn_list, &barn->sheaves_full);
	barn->nr_full++;
}

/* Trade an empty sheaf for a full one, if the barn has one. */
static struct slab_sheaf *barn_replace_empty_sheaf(struct node_barn *barn,
						   struct slab_sheaf *empty)
{
	struct slab_sheaf *full = barn_take(&barn->sheaves_full,
					    &barn->nr_full);

	if (full)
		barn_put_empty_sheaf(barn, empty);
	return full;
}

/* The main sheaf is empty: make it non-empty, or fail. */
static bool replace_empty_main(struct kmem_cache *s,
			       struct slub_percpu_sheaves *pcs)
{
	struct node_barn *barn = &s->barn;
	struct slab_sheaf *full, *empty;

	if (pcs->spare && pcs->spare->size > 0) {	/* 1. use the spare */
		swap(pcs->main, pcs->spare);
		return true;
	}
	full = barn_replace_empty_sheaf(barn, pcs->main);
	if (full) {					/* 2. trade at the barn */
		s->stat.barn_get++;
		pcs->main = full;
		return true;
	}
	/* 3. Fill an empty sheaf from the slabs. */
	if (pcs->spare) {
		empty = pcs->spare;
		pcs->spare = NULL;
	} else {
		empty = barn_get_empty_sheaf(barn);
		if (!empty)
			empty = alloc_empty_sheaf(s);
		if (!empty)
			return false;
	}
	if (refill_sheaf(s, empty)) {
		sheaf_flush(s, empty);
		free_empty_sheaf(empty);
		return false;
	}
	barn_put_empty_sheaf(barn, pcs->main);
	pcs->main = empty;
	return true;
}

static void *alloc_from_pcs(struct kmem_cache *s)
{
	struct slub_percpu_sheaves *pcs = this_cpu_sheaves(s);
	void *object = NULL;

	if (!local_trylock(pcs))
		return NULL;
	if (pcs->main->size > 0 || replace_empty_main(s, pcs)) {
		object = pcs->main->objects[--pcs->main->size];
		s->stat.alloc_pcs++;
	}
	local_unlock(pcs);
	return object;
}

/* The main sheaf is full: make room in it, or fail. */
static bool replace_full_main(struct kmem_cache *s,
			      struct slub_percpu_sheaves *pcs)
{
	struct node_barn *barn = &s->barn;
	struct slab_sheaf *empty;

	if (pcs->spare && pcs->spare->size < s->sheaf_capacity) {
		swap(pcs->main, pcs->spare);	/* 1. use the spare */
		return true;
	}
	if (pcs->spare && barn->nr_full >= MAX_FULL_SHEAVES) {
		sheaf_flush(s, pcs->spare);	/* 2. the barn is at its limit */
		swap(pcs->main, pcs->spare);
		return true;
	}
	/* 3. Get an empty sheaf; park the full main as spare or in the barn. */
	empty = barn_get_empty_sheaf(barn);
	if (!empty)
		empty = alloc_empty_sheaf(s);
	if (!empty)
		return false;
	if (!pcs->spare) {
		pcs->spare = pcs->main;
	} else {
		barn_put_full_sheaf(barn, pcs->main);
		s->stat.barn_put++;
	}
	pcs->main = empty;
	return true;
}

static bool free_to_pcs(struct kmem_cache *s, void *object)
{
	struct slub_percpu_sheaves *pcs = this_cpu_sheaves(s);
	bool done = false;

	if (!local_trylock(pcs))
		return false;
	if (pcs->main->size < s->sheaf_capacity || replace_full_main(s, pcs)) {
		pcs->main->objects[pcs->main->size++] = object;
		s->stat.free_pcs++;
		done = true;
	}
	local_unlock(pcs);
	return done;
}

/* ------------------------------------------------------------------ */
/* Prefilled sheaves.                                                  */

struct slab_sheaf *ms_prefill_sheaf(struct kmem_cache *s, unsigned int count)
{
	struct slub_percpu_sheaves *pcs = this_cpu_sheaves(s);
	struct slab_sheaf *sheaf;

	if (!s->sheaf_capacity || count > s->sheaf_capacity)
		return NULL;	/* the kernel allocates an oversize sheaf */
	sheaf = pcs->spare;
	pcs->spare = NULL;
	if (!sheaf)
		sheaf = barn_take(&s->barn.sheaves_full, &s->barn.nr_full);
	if (!sheaf)
		sheaf = barn_get_empty_sheaf(&s->barn);
	if (!sheaf)
		sheaf = alloc_empty_sheaf(s);
	if (!sheaf)
		return NULL;
	if (sheaf->size < count && refill_sheaf(s, sheaf)) {
		sheaf_flush(s, sheaf);
		free_empty_sheaf(sheaf);
		return NULL;
	}
	return sheaf;
}

static void post_alloc_hook(struct kmem_cache *s, void *object);

/* Cannot fail while the sheaf has objects; never falls back to slabs. */
void *ms_alloc_from_sheaf(struct kmem_cache *s, struct slab_sheaf *sheaf)
{
	void *object;

	if (sheaf->size == 0)
		return NULL;
	object = sheaf->objects[--sheaf->size];
	post_alloc_hook(s, object);
	return object;
}

void ms_return_sheaf(struct kmem_cache *s, struct slab_sheaf *sheaf)
{
	struct slub_percpu_sheaves *pcs = this_cpu_sheaves(s);

	if (!pcs->spare) {
		pcs->spare = sheaf;
		return;
	}
	if (s->barn.nr_full >= MAX_FULL_SHEAVES || refill_sheaf(s, sheaf)) {
		sheaf_flush(s, sheaf);
		free_empty_sheaf(sheaf);
		return;
	}
	barn_put_full_sheaf(&s->barn, sheaf);
}

/* ------------------------------------------------------------------ */
/* The cache API.                                                      */

static void post_alloc_hook(struct kmem_cache *s, void *object)
{
	if (!(s->flags & MS_POISON))
		return;
	for (unsigned int i = 0; i < s->object_size; i++)
		if (((unsigned char *)object)[i] != POISON_FREE)
			ms_bug(s, "free object was written to");
}

void *ms_cache_alloc(struct kmem_cache *s)
{
	void *object = NULL;

	if (s->sheaf_capacity)
		object = alloc_from_pcs(s);
	if (!object)
		object = slab_alloc(s);
	if (object)
		post_alloc_hook(s, object);
	return object;
}

void ms_cache_free(struct kmem_cache *s, void *object)
{
	struct slab *slab = virt_to_slab(object);

	if (!slab || slab->cache != s ||
	    ((char *)object - (char *)slab->base) % s->size)
		ms_bug(s, "freeing a pointer that is not an object of this cache");
	poison_object(s, object);
	if (s->sheaf_capacity && free_to_pcs(s, object))
		return;
	do_slab_free(s, slab, object);
}

/* Slab order: the smallest order with enough objects and <= 1/16 waste. */
static unsigned int calculate_order(unsigned int size)
{
	unsigned int min_objects = 4 * (fls(NR_CPUS) + 1);
	unsigned int max_objects = (PAGE_SIZE << MAX_SLAB_ORDER) / size;
	unsigned int min_order, order;

	if (max_objects < 1)
		max_objects = 1;
	if (min_objects > max_objects)
		min_objects = max_objects;
	min_order = get_order((unsigned long)min_objects * size);
	for (unsigned int fraction = 16; fraction > 1; fraction /= 2) {
		for (order = min_order; order <= MAX_SLAB_ORDER; order++) {
			unsigned long slab_size = PAGE_SIZE << order;

			if (slab_size % size <= slab_size / fraction)
				return order;
		}
	}
	return get_order(size);
}

struct kmem_cache *ms_cache_create(const char *name, unsigned int object_size,
				   unsigned int align, unsigned int flags,
				   unsigned int sheaf_capacity)
{
	struct kmem_cache *s = calloc(1, sizeof(*s));
	unsigned int size, nr_objects;

	if (!s)
		return NULL;
	if (align < sizeof(void *))
		align = sizeof(void *);
	s->name = name;
	s->object_size = object_size;
	s->align = align;
	s->flags = flags;
	s->random = ((uintptr_t)ms_random_u32() << 31 << 1) | ms_random_u32();

	size = ALIGN(object_size, (unsigned int)sizeof(void *));
	if (flags & MS_POISON) {
		s->offset = size;		/* free pointer after the object */
		size += sizeof(void *);
	} else {				/* near the middle of the object */
		s->offset = ALIGN_DOWN(object_size / 2,
				       (unsigned int)sizeof(void *));
	}
	s->size = ALIGN(size, align);
	s->order = calculate_order(s->size);
	s->objects = (PAGE_SIZE << s->order) / s->size;

	/* As in the kernel: 5..10 empty-able slabs, sized by object size. */
	s->min_partial = ilog2(s->size) / 2;
	s->min_partial = s->min_partial < 5 ? 5 : s->min_partial > 10 ?
			 10 : s->min_partial;
	nr_objects = s->size >= PAGE_SIZE ? 6 : s->size >= 1024 ? 24 :
		     s->size >= 256 ? 52 : 120;
	s->cpu_partial_slabs = DIV_ROUND_UP(nr_objects * 2, s->objects);

	if (flags & MS_RANDOM) {
		s->random_seq = calloc(s->objects, sizeof(unsigned int));
		freelist_randomize(s->random_seq, s->objects);
		for (unsigned int i = 0; i < s->objects; i++)
			s->random_seq[i] *= s->size;	/* index -> offset */
	}
	list_init(&s->partial);
	list_init(&s->barn.sheaves_full);
	list_init(&s->barn.sheaves_empty);
	for (int cpu = 0; cpu < NR_CPUS; cpu++)
		s->cpu_slab[cpu].tid = cpu;		/* init_tid() */
	s->sheaf_capacity = sheaf_capacity;
	if (sheaf_capacity)
		for (int cpu = 0; cpu < NR_CPUS; cpu++)
			s->cpu_sheaves[cpu].main = alloc_empty_sheaf(s);
	return s;
}

/* Give back everything cached: sheaves, CPU slabs, empty slabs. */
void ms_cache_shrink(struct kmem_cache *s)
{
	int saved = ms_cpu;
	struct slab_sheaf *sheaf;
	struct slab *slab, *tmp;

	while ((sheaf = barn_take(&s->barn.sheaves_full, &s->barn.nr_full))) {
		sheaf_flush(s, sheaf);
		free_empty_sheaf(sheaf);
	}
	while ((sheaf = barn_get_empty_sheaf(&s->barn)))
		free_empty_sheaf(sheaf);
	for (ms_cpu = 0; ms_cpu < NR_CPUS; ms_cpu++) {
		struct slub_percpu_sheaves *pcs = this_cpu_sheaves(s);
		struct kmem_cache_cpu *c = this_cpu_ptr(s);

		if (s->sheaf_capacity) {
			sheaf_flush(s, pcs->main);
			if (pcs->spare) {
				sheaf_flush(s, pcs->spare);
				free_empty_sheaf(pcs->spare);
				pcs->spare = NULL;
			}
		}
		if (c->slab) {
			slab = c->slab;
			c->slab = NULL;
			deactivate_slab(s, slab, c->freelist);
			c->freelist = NULL;
			c->tid = next_tid(c->tid);
		}
		slab = c->partial;
		c->partial = NULL;
		put_partials(s, slab);
	}
	ms_cpu = saved;
	list_for_each_entry_safe(slab, tmp, &s->partial, slab_list) {
		if (!slab->inuse) {
			remove_partial(s, slab);
			discard_slab(s, slab);
		}
	}
}

void ms_cache_destroy(struct kmem_cache *s)
{
	ms_cache_shrink(s);
	if (s->nr_partial)
		ms_bug(s, "destroying a cache with live objects");
	for (int cpu = 0; cpu < NR_CPUS; cpu++)
		free_empty_sheaf(s->cpu_sheaves[cpu].main);
	free(s->random_seq);
	free(s);
}

/* ------------------------------------------------------------------ */
/* kmalloc.                                                            */

/* Size class for sizes up to 192, in steps of 8 (index 1 = 96, 2 = 192). */
static const unsigned char kmalloc_size_index[24] = {
	3, 4, 5, 5, 6, 6, 6, 6,		/*   8 ..  64 */
	1, 1, 1, 1, 7, 7, 7, 7,		/*  72 .. 128 */
	2, 2, 2, 2, 2, 2, 2, 2,		/* 136 .. 192 */
};

static struct kmem_cache *kmalloc_caches[KMALLOC_SHIFT_HIGH + 1];
static char kmalloc_names[KMALLOC_SHIFT_HIGH + 1][16];

unsigned int kmalloc_index(size_t size)
{
	if (size <= 192)
		return kmalloc_size_index[(size - 1) / 8];
	return fls(size - 1);
}

struct kmem_cache *kmalloc_cache(unsigned int index)
{
	return index <= KMALLOC_SHIFT_HIGH ? kmalloc_caches[index] : NULL;
}

void *ms_kmalloc(size_t size)
{
	if (!size)
		return NULL;		/* the kernel returns ZERO_SIZE_PTR */
	if (size > KMALLOC_MAX_CACHE_SIZE)
		return alloc_pages(get_order(size));	/* straight to pages */
	return ms_cache_alloc(kmalloc_caches[kmalloc_index(size)]);
}

void ms_kfree(const void *p)
{
	struct slab *slab;
	struct page *pg;

	if (!p)
		return;
	slab = virt_to_slab(p);
	if (slab) {
		ms_cache_free(slab->cache, (void *)p);
		return;
	}
	pg = virt_to_page(p);
	if (!pg || !pg->used || pg->head != pg)
		ms_bug(NULL, "kfree() of a pointer kmalloc() did not return");
	free_pages((void *)p, pg->order);
}

/* ------------------------------------------------------------------ */

void ms_init(unsigned long npages, uint64_t seed)
{
	static const unsigned int sizes[] = { 0, 96, 192 };

	rng_state = seed;
	nr_pages = npages;
	next_fit = 0;
	pages_in_use = 0;
	arena = aligned_alloc(PAGE_SIZE, npages * PAGE_SIZE);
	pages = calloc(npages, sizeof(*pages));
	if (!arena || !pages)
		ms_bug(NULL, "out of memory for the arena");
	for (unsigned int i = 1; i <= KMALLOC_SHIFT_HIGH; i++) {
		unsigned int size = i < 3 ? sizes[i] : 1U << i;

		snprintf(kmalloc_names[i], sizeof(kmalloc_names[i]),
			 "kmalloc-%u", size);
		kmalloc_caches[i] = ms_cache_create(kmalloc_names[i], size,
						    0, 0, 0);
	}
}

void ms_exit(void)
{
	for (unsigned int i = 1; i <= KMALLOC_SHIFT_HIGH; i++) {
		ms_cache_destroy(kmalloc_caches[i]);
		kmalloc_caches[i] = NULL;
	}
	free(arena);
	free(pages);
	arena = NULL;
	pages = NULL;
}
