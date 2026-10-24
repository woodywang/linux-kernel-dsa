// SPDX-License-Identifier: MIT
/*
 * test_mini_slub.c - randomized test of mini_slub.c against a model.
 *
 *   cc -O2 -Wall -Wextra -fsanitize=address,undefined -o test_mini_slub \
 *      test_mini_slub.c
 *   ./test_mini_slub [operations] [seed]
 *
 * The model is the set of live objects: where each one is, how big it is,
 * and the byte pattern written into it.  AddressSanitizer cannot see into
 * the allocator's own pages (to ASan the whole arena is one live block),
 * so the test checks what ASan would otherwise catch:
 *   - every object handed out lies inside a slab of the right cache, at an
 *     object boundary, aligned, and overlaps no live object (a bitmap of
 *     8-byte granules marks live memory);
 *   - each object's pattern is intact when it is freed, and in periodic
 *     full scans (so nothing wrote into a live object);
 *   - caches created with MS_POISON fill free objects with 0x6b and the
 *     allocator aborts if a free object was modified;
 *   - after each batch of operations, the allocator's own bookkeeping is
 *     consistent: free lists hold distinct, non-live objects of their slab;
 *     inuse counts match; every slab is in exactly one state; and
 *       live objects = sum(inuse) - CPU free lists - objects in sheaves;
 *   - at the end, after freeing everything and shrinking, every page has
 *     gone back to the page source.
 * At the point in each fast path where the kernel could be interrupted or
 * migrated, a hook sometimes runs a few nested allocations and frees on the
 * same CPU (an "interrupt") or moves to another CPU (a "migration", only
 * when no per-CPU lock is held), so the transaction-id retry paths run too.
 */
#include "mini_slub.c"

#include <assert.h>
#include <inttypes.h>

#define ARENA_PAGES	16384		/* 64 MiB */
#define MAX_LIVE	6000
#define HEADROOM	64		/* slots kept for "interrupts" */
#define NCACHES		(5 + KMALLOC_SHIFT_HIGH)

struct obj {
	char *p;
	size_t size;
	uint32_t seed;
	int cache;			/* index in caches[], -1: large kmalloc */
};

static struct obj live[MAX_LIVE];
static int nlive;
static struct kmem_cache *caches[NCACHES];
static int ncaches, ntest_caches;
static long live_count[NCACHES];
static unsigned char *granule_live, *granule_seen;	/* bitmaps */
static unsigned long checks;

static uint64_t trng = 88172645463325252ULL;	/* the test's own RNG */
static uint32_t rnd(void)
{
	trng ^= trng << 13;
	trng ^= trng >> 7;
	trng ^= trng << 17;
	return (uint32_t)(trng >> 16);
}

static unsigned long granule(const void *p)
{
	return (unsigned long)((const char *)p - arena) / 8;
}

static bool test_bit(const unsigned char *map, unsigned long i)
{
	return map[i / 8] & (1u << (i % 8));
}

static void set_bits(unsigned char *map, unsigned long i, unsigned long n,
		     bool val)
{
	for (; n; i++, n--) {
		if (val)
			map[i / 8] |= 1u << (i % 8);
		else
			map[i / 8] &= ~(1u << (i % 8));
	}
}

static int cache_index(const struct kmem_cache *s)
{
	for (int i = 0; i < ncaches; i++)
		if (caches[i] == s)
			return i;
	assert(!"unknown cache");
	return -1;
}

static void fill(struct obj *o)
{
	for (size_t i = 0; i < o->size; i++)
		o->p[i] = (char)(o->seed + i * 31);
}

static void verify(const struct obj *o)
{
	for (size_t i = 0; i < o->size; i++)
		assert(o->p[i] == (char)(o->seed + i * 31));
	checks++;
}

/* A new object: in bounds, aligned, not overlapping anything live. */
static void track(char *p, size_t size, int cache)
{
	struct obj *o = &live[nlive++];
	unsigned long g = granule(p), n = (size + 7) / 8;

	assert(p);
	if (cache >= 0) {
		struct kmem_cache *s = caches[cache];
		struct slab *slab = virt_to_slab(p);

		assert(slab && slab->cache == s);
		assert((p - (char *)slab->base) % s->size == 0);
		assert((size_t)(p - (char *)slab->base) / s->size < slab->objects);
		assert(((uintptr_t)p & (s->align - 1)) == 0);
		assert(size <= s->object_size);
		live_count[cache]++;
	} else {
		assert(((uintptr_t)p & (PAGE_SIZE - 1)) == 0);
		assert(!virt_to_slab(p));
	}
	for (unsigned long i = 0; i < n; i++)
		assert(!test_bit(granule_live, g + i));
	set_bits(granule_live, g, n, true);
	*o = (struct obj){ p, size, rnd(), cache };
	fill(o);
}

static void untrack(int i)
{
	struct obj *o = &live[i];

	verify(o);
	set_bits(granule_live, granule(o->p), (o->size + 7) / 8, false);
	if (o->cache >= 0)
		live_count[o->cache]--;
	live[i] = live[--nlive];
}

/* A free object found in the allocator: of this cache, not live, unseen. */
static void check_free_object(struct kmem_cache *s, struct slab *slab,
			      void *obj)
{
	unsigned long g = granule(obj);

	if (slab)
		assert(virt_to_slab(obj) == slab);
	else
		slab = virt_to_slab(obj);
	assert(slab && slab->cache == s);
	assert(((char *)obj - (char *)slab->base) % s->size == 0);
	assert(!test_bit(granule_live, g));
	assert(!test_bit(granule_seen, g));
	set_bits(granule_seen, g, 1, true);
}

static unsigned long walk_freelist(struct kmem_cache *s, struct slab *slab,
				   void *p)
{
	unsigned long n = 0;

	for (; p; p = get_freepointer(s, p), n++) {
		assert(n < slab->objects);
		check_free_object(s, slab, p);
	}
	return n;
}

static unsigned long walk_sheaf(struct kmem_cache *s, struct slab_sheaf *sh)
{
	assert(sh->size <= s->sheaf_capacity);
	for (unsigned int i = 0; i < sh->size; i++)
		check_free_object(s, NULL, sh->objects[i]);
	return sh->size;
}

static void validate(int ci)
{
	struct kmem_cache *s = caches[ci];
	unsigned long nslabs = 0, nfrozen = 0, npartial = 0, nfull = 0;
	unsigned long ncpupartial = 0, inuse = 0, cached = 0, nempty = 0;
	struct slab_sheaf *sh;
	struct slab *slab;

	memset(granule_seen, 0, nr_pages * PAGE_SIZE / 64);
	for (unsigned long pfn = 0; pfn < nr_pages; pfn++) {
		struct page *pg = &pages[pfn];

		if (!pg->used || pg->head != pg || pg->slab.cache != s)
			continue;
		slab = &pg->slab;
		nslabs++;
		assert(slab->base == arena + (pfn << PAGE_SHIFT));
		assert(slab->objects == s->objects && slab->inuse <= s->objects);
		assert(walk_freelist(s, slab, slab->freelist) ==
		       slab->objects - slab->inuse);
		inuse += slab->inuse;
		assert(!(slab->frozen && slab->on_partial));
		if (slab->frozen)
			nfrozen++;
		else if (!slab->on_partial && !slab->freelist)
			nfull++;
	}
	for (int cpu = 0; cpu < NR_CPUS; cpu++) {
		struct kmem_cache_cpu *c = &s->cpu_slab[cpu];
		unsigned int len = 0;

		assert(c->tid % TID_STEP == (unsigned long)cpu);
		if (c->freelist)
			assert(c->slab);
		if (c->slab) {
			assert(c->slab->cache == s && c->slab->frozen);
			cached += walk_freelist(s, c->slab, c->freelist);
			nfrozen--;
		}
		for (slab = c->partial; slab; slab = slab->next, len++) {
			assert(slab->cache == s && !slab->frozen);
			assert(!slab->on_partial && slab->freelist);
		}
		if (c->partial)
			assert(c->partial->slabs == len);
		assert(len <= (s->cpu_partial_slabs ? s->cpu_partial_slabs : 1));
		ncpupartial += len;
		if (s->sheaf_capacity) {
			struct slub_percpu_sheaves *pcs = &s->cpu_sheaves[cpu];

			assert(pcs->main);
			cached += walk_sheaf(s, pcs->main);
			if (pcs->spare)
				cached += walk_sheaf(s, pcs->spare);
		}
	}
	assert(nfrozen == 0);		/* every frozen slab is some CPU's */
	list_for_each_entry(slab, &s->partial, slab_list) {
		assert(slab->on_partial && !slab->frozen && slab->freelist);
		npartial++;
		nempty += !slab->inuse;
	}
	assert(nempty <= s->min_partial);	/* empty slabs are released */
	assert(npartial == s->nr_partial);
	for (int cpu = 0; cpu < NR_CPUS; cpu++)
		if (s->cpu_slab[cpu].slab)
			nfrozen++;
	assert(nslabs == nfrozen + npartial + ncpupartial + nfull);

	unsigned int nfullsh = 0, nemptysh = 0;

	list_for_each_entry(sh, &s->barn.sheaves_full, barn_list) {
		assert(sh->size == s->sheaf_capacity);
		cached += walk_sheaf(s, sh);
		nfullsh++;
	}
	list_for_each_entry(sh, &s->barn.sheaves_empty, barn_list) {
		assert(sh->size == 0);
		nemptysh++;
	}
	assert(nfullsh == s->barn.nr_full && nemptysh == s->barn.nr_empty);
	assert(s->barn.nr_full <= MAX_FULL_SHEAVES + NR_CPUS);

	/* The key invariant: the model and the allocator agree. */
	assert((long)(inuse - cached) == live_count[ci]);
	assert(s->stat.new_slabs - s->stat.freed_slabs == nslabs);
}

static void validate_all(void)
{
	for (int i = 0; i < ncaches; i++)
		validate(i);
}

static int cur_cache = -1;	/* the cache the current operation uses */

/* Allocate from caches[ci] (a kmalloc cache is reached via ms_kmalloc). */
static void alloc_in(int ci, size_t size)
{
	cur_cache = ci;
	if (ci < ntest_caches) {
		track(ms_cache_alloc(caches[ci]), size, ci);
	} else {
		char *p = ms_kmalloc(size);
		struct slab *slab = virt_to_slab(p);

		assert(slab && slab->cache == caches[ci]);
		assert(slab->cache == kmalloc_cache(kmalloc_index(size)));
		assert(slab->cache->object_size >= size);
		track(p, size, ci);
	}
}

static void do_alloc(void)
{
	if (rnd() % 10 < 7) {
		int ci = rnd() % ntest_caches;

		alloc_in(ci, 1 + rnd() % caches[ci]->object_size);
	} else {
		unsigned int r = rnd() % 100;
		size_t size = r < 80 ? 1 + rnd() % 512 :
			      r < 95 ? 513 + rnd() % 7680 : 8193 + rnd() % 30000;

		if (size <= KMALLOC_MAX_CACHE_SIZE) {
			alloc_in(cache_index(kmalloc_cache(kmalloc_index(size))),
				 size);
		} else {
			cur_cache = -1;
			track(ms_kmalloc(size), size, -1);
		}
	}
}

static void free_at(int i)
{
	struct obj o = live[i];

	untrack(i);
	cur_cache = o.cache;
	if (o.cache < 0 || o.cache >= ntest_caches)
		ms_kfree(o.p);
	else
		ms_cache_free(caches[o.cache], o.p);
}

static void do_free(void)
{
	free_at(rnd() % nlive);
}

static bool hook_enabled;
static unsigned long interrupts, migrations;

/* Runs where the kernel's fast path could be interrupted or migrated. */
static void interrupt_hook(void)
{
	static bool in_hook;

	if (!hook_enabled || in_hook || rnd() % 16)
		return;
	in_hook = true;
	if (rnd() % 4 == 0 && ms_preempt_count == 0) {
		ms_cpu = rnd() % NR_CPUS;
		migrations++;
	} else if (cur_cache >= 0) {
		/* Interrupt handlers often allocate and soon free; doing so on
		 * the interrupted cache is what creates ABA hazards. */
		int ci = cur_cache;

		for (int n = 1 + rnd() % 4; n; n--) {
			if (rnd() % 2 && nlive < MAX_LIVE - 1)
				alloc_in(ci, caches[ci]->object_size);
			else if (nlive)		/* one of the newest */
				free_at(nlive - 1 -
					rnd() % (nlive < 3 ? nlive : 3));
		}
		cur_cache = ci;
		interrupts++;
	}
	in_hook = false;
}

/* Borrow a prefilled sheaf, allocate up to @count from it, return it. */
static void do_prefill(void)
{
	int ci = rnd() % ntest_caches;
	struct kmem_cache *s = caches[ci];
	unsigned int count, k;
	struct slab_sheaf *sheaf;

	if (!s->sheaf_capacity ||
	    nlive + (int)s->sheaf_capacity >= MAX_LIVE - HEADROOM)
		return;
	count = 1 + rnd() % s->sheaf_capacity;
	sheaf = ms_prefill_sheaf(s, count);
	assert(sheaf && sheaf->size >= count);
	k = rnd() % (count + 1);
	for (unsigned int i = 0; i < k; i++)
		track(ms_alloc_from_sheaf(s, sheaf), s->object_size, ci);
	ms_return_sheaf(s, sheaf);
}

int main(int argc, char **argv)
{
	long ops = argc > 1 ? atol(argv[1]) : 300000;
	uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;

	trng ^= seed * 0x9e3779b97f4a7c15ULL;
	ms_init(ARENA_PAGES, seed);
	granule_live = calloc(ARENA_PAGES * PAGE_SIZE / 64, 1);
	granule_seen = calloc(ARENA_PAGES * PAGE_SIZE / 64, 1);
	assert(granule_live && granule_seen);

	caches[ncaches++] = ms_cache_create("tiny", 8, 0, 0, 0);
	caches[ncaches++] = ms_cache_create("obj-24", 24, 8,
					    MS_RANDOM | MS_POISON, 4);
	caches[ncaches++] = ms_cache_create("obj-200", 200, 64, MS_RANDOM, 32);
	caches[ncaches++] = ms_cache_create("obj-3000", 3000, 0, MS_POISON, 0);
	caches[ncaches++] = ms_cache_create("obj-48", 48, 16,
					    MS_RANDOM | MS_POISON, 0);
	ntest_caches = ncaches;
	for (unsigned int i = 1; i <= KMALLOC_SHIFT_HIGH; i++)
		caches[ncaches++] = kmalloc_cache(i);
	ms_preempt_hook = interrupt_hook;
	hook_enabled = true;

	for (long it = 0; it < ops; it++) {
		int p_alloc = (it / 20000) % 2 ? 35 : 62;
		int r = rnd() % 100;

		if (rnd() % 8 == 0)
			ms_cpu = rnd() % NR_CPUS;
		if (r < p_alloc && nlive < MAX_LIVE - HEADROOM)
			do_alloc();
		else if (r < 97 && nlive)
			do_free();
		else if (r < 99)
			do_prefill();
		else {
			hook_enabled = false;	/* shrinking visits every CPU */
			ms_cache_shrink(caches[rnd() % ncaches]);
			hook_enabled = true;
		}
		if (it % 1000 == 999)
			validate_all();
		if (it % 20000 == 19999)
			for (int i = 0; i < nlive; i++)
				verify(&live[i]);
	}
	hook_enabled = false;
	while (nlive)
		do_free();
	for (int i = 0; i < ncaches; i++)
		ms_cache_shrink(caches[i]);
	validate_all();
	assert(ms_pages_in_use() == 0);

	unsigned long fast = 0, slow = 0, pcs = 0, slabs = 0, retries = 0;

	for (int i = 0; i < ncaches; i++) {
		struct ms_stats *st = &caches[i]->stat;

		fast += st->alloc_fast;
		slow += st->alloc_slow;
		pcs += st->alloc_pcs;
		slabs += st->new_slabs;
		retries += st->cmpxchg_fail;
	}
	printf("test_mini_slub: %ld ops, seed %" PRIu64 ": ok\n"
	       "  %lu objects checked; allocations: %lu sheaf, %lu fast, "
	       "%lu slow\n"
	       "  %lu interrupts, %lu migrations, %lu cmpxchg retries; "
	       "%lu slabs created, all pages returned\n",
	       ops, seed, checks, pcs, fast, slow, interrupts, migrations,
	       retries, slabs);
	for (int i = 0; i < ntest_caches; i++)
		ms_cache_destroy(caches[i]);
	ms_exit();
	free(granule_live);
	free(granule_seen);
	return 0;
}
