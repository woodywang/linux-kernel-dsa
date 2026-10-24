// SPDX-License-Identifier: GPL-2.0+
/*
 * maple_alloc_demo.c - Chapter 8 exercise: a toy virtual-address-space
 * allocator built on the kernel's own maple tree, compiled in userspace
 * via tools/testing/radix-tree.
 *
 * Build (from tools/testing/radix-tree of a v6.18 tree):
 *   cp /path/to/maple_alloc_demo.c .
 *   make maple            # builds the shared objects once
 *   cc -I../shared -I. -I../../include -I../../arch/x86/include \
 *       -I../../../lib -g -Og -Wall -D_LGPL_SOURCE \
 *      -fsanitize=address -fsanitize=undefined \
 *      -c maple_alloc_demo.c -o maple_alloc_demo.o
 *   cc maple_alloc_demo.o xarray-shared.o radix-tree.o idr.o linux.o \
 *      slab.o find_bit.o bitmap.o hweight.o vsprintf.o test.o \
 *      -fsanitize=address -fsanitize=undefined -lpthread -lurcu \
 *      -o maple_alloc_demo
 */
#include "test.h"
#include "../shared/maple-shim.c"

#define PAGE_SZ		0x1000UL
#define LOW_LIMIT	0x10000UL		/* like mmap_min_addr */
#define HIGH_LIMIT	0x7fffffff0000UL	/* top of a toy address space */

struct region {
	unsigned long start, end;	/* [start, end) like vm_start/vm_end */
	char name[16];
};

/* A tree with the same flags the kernel uses for mm->mm_mt, minus the
 * external lock: track gaps (ALLOC_RANGE) and run in RCU mode. */
static DEFINE_MTREE(as);

/*
 * Top-down "mmap": find the highest gap of @len bytes below HIGH_LIMIT,
 * the same search unmapped_area_topdown() performs with
 * vma_iter_area_highest() -> mas_empty_area_rev().
 */
static struct region *toy_mmap(unsigned long len, const char *name)
{
	MA_STATE(mas, &as, 0, 0);
	struct region *r;
	int ret;

	len = (len + PAGE_SZ - 1) & ~(PAGE_SZ - 1);
	mtree_lock(&as);
	ret = mas_empty_area_rev(&mas, LOW_LIMIT, HIGH_LIMIT - 1, len);
	if (ret) {
		mtree_unlock(&as);
		return NULL;
	}
	r = malloc(sizeof(*r));
	r->start = mas.index;			/* gap is [index, last] */
	r->end = mas.last + 1;
	snprintf(r->name, sizeof(r->name), "%s", name);
	/* Store the entry over the whole range: one write covers it all. */
	mas_set_range(&mas, r->start, r->end - 1);
	ret = mas_store_gfp(&mas, r, GFP_KERNEL);
	mtree_unlock(&as);
	if (ret) {
		free(r);
		return NULL;
	}
	return r;
}

/* Fixed-address "mmap": refuse if anything is already mapped there. */
static int toy_mmap_fixed(struct region *r)
{
	return mtree_insert_range(&as, r->start, r->end - 1, r, GFP_KERNEL);
}

static void toy_munmap(struct region *r)
{
	/* Erasing by any index inside the range removes the whole range. */
	struct region *old = mtree_erase(&as, r->start + PAGE_SZ / 2);

	assert(old == r);
	free(r);
}

/* find_vma(): the region containing addr, or the next one above it. */
static struct region *toy_find(unsigned long addr)
{
	unsigned long index = addr;

	return mt_find(&as, &index, ULONG_MAX);
}

static void dump_regions(const char *title)
{
	MA_STATE(mas, &as, 0, 0);
	struct region *r;
	int n = 0;

	printf("-- %s (height %u)\n", title, mt_height(&as));
	rcu_read_lock();
	mas_for_each(&mas, r, ULONG_MAX) {
		printf("   [%#014lx-%#014lx] %-8s (tree range %#lx-%#lx)\n",
		       r->start, r->end, r->name, mas.index, mas.last);
		n++;
	}
	rcu_read_unlock();
	printf("   %d regions\n", n);
}

int main(void)
{
	struct region *heap, *libc, *stack, *r, *fixed;
	unsigned long i;
	int n;

	maple_tree_init();
	mt_init_flags(&as, MT_FLAGS_ALLOC_RANGE | MT_FLAGS_USE_RCU);

	stack = toy_mmap(8 * 1024 * 1024, "stack");
	libc  = toy_mmap(2 * 1024 * 1024, "libc");
	heap  = toy_mmap(64 * 1024, "heap");
	dump_regions("three top-down mappings");
	printf("heap sits below libc? %s\n", heap->end == libc->start ? "yes" : "no");

	/* A fixed mapping that collides must fail with -EEXIST. */
	fixed = malloc(sizeof(*fixed));
	*fixed = (struct region){ libc->start, libc->start + PAGE_SZ, "clash" };
	printf("fixed map over libc: %d (expect %d)\n",
	       toy_mmap_fixed(fixed), -EEXIST);
	fixed->start = LOW_LIMIT;
	fixed->end = LOW_LIMIT + 16 * PAGE_SZ;
	snprintf(fixed->name, sizeof(fixed->name), "text");
	printf("fixed map at LOW_LIMIT: %d (expect 0)\n", toy_mmap_fixed(fixed));

	/* Punch a hole: unmap libc, then the next top-down mmap that fits
	 * reuses the hole because it is the highest suitable gap. */
	toy_munmap(libc);
	r = toy_mmap(PAGE_SZ, "anon");
	printf("anon reused libc hole? %s\n",
	       r->end == stack->start ? "yes, right below stack" : "no");

	r = toy_find(LOW_LIMIT + 3 * PAGE_SZ);
	printf("find(text+3 pages) -> %s\n", r ? r->name : "(none)");
	r = toy_find(LOW_LIMIT + 64 * PAGE_SZ);
	printf("find(gap above text) -> next region %s\n", r ? r->name : "(none)");

	/* Grow the tree until it needs several levels. */
	for (i = 0, n = 0; i < 2000; i++)
		n += toy_mmap(PAGE_SZ * (1 + i % 7), "small") != NULL;
	printf("placed %d more mappings: height %u\n", n, mt_height(&as));
	mt_validate(&as);	/* checks pivots, gaps and parent pointers */

	/* Tear down. */
	{
		MA_STATE(mas, &as, 0, 0);

		mas_lock(&mas);
		mas_for_each(&mas, r, ULONG_MAX)
			free(r);
		mas_unlock(&mas);
	}
	mtree_destroy(&as);
	rcu_barrier();	/* let RCU-deferred node frees run */
	/* AddressSanitizer's leak checker reports anything left at exit. */
	printf("done\n");
	return 0;
}
