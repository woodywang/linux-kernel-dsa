// SPDX-License-Identifier: MIT
/*
 * htab_demo.c - what preallocation means for a small BPF-style hash map.
 * The output is deterministic; Section 20.3 quotes it.
 */
#include "htab.h"

#include <assert.h>
#include <stdio.h>

static long index_of(const struct htab *h, const void *value)
{
	/* Which preallocated element holds this value? */
	return ((const char *)value - h->elems) / h->elem_size;
}

static void show_free(const struct htab *h)
{
	printf("free elements per CPU:");
	for (int cpu = 0; cpu < NR_CPUS; cpu++)
		printf(" %d", htab_free_count(h, cpu));
	printf("\n");
}

int main(void)
{
	struct htab *h = htab_create(4, 8, 8, 0);
	unsigned long mallocs = htab_nr_mallocs;
	uint64_t v = 1;
	uint32_t key;
	int ret;

	assert(h);
	printf("max_entries 8, %d CPUs: %d elements preallocated, "
	       "%u buckets\n", NR_CPUS, 8 + NR_CPUS, h->n_buckets);
	show_free(h);

	htab_cpu = 1;
	for (key = 0; key < 8; key++)
		assert(htab_update(h, &key, &v, BPF_ANY) == 0);
	ret = htab_update(h, &key, &v, BPF_ANY);
	printf("CPU 1 inserts keys 0-7: ok; key 8: %d (-E2BIG)\n", ret);
	show_free(h);

	key = 3;
	long before = index_of(h, htab_lookup(h, &key));

	ret = htab_update(h, &key, &v, BPF_EXIST);
	printf("update key 3 in the full map: %d; element %ld -> %ld "
	       "(CPU 1's spare)\n", ret, before,
	       index_of(h, htab_lookup(h, &key)));

	key = 5;
	before = index_of(h, htab_lookup(h, &key));
	htab_delete(h, &key);
	key = 9;
	htab_update(h, &key, &v, BPF_NOEXIST);
	printf("delete key 5 (element %ld), insert key 9: element %ld\n",
	       before, index_of(h, htab_lookup(h, &key)));
	printf("malloc() calls after creation: %lu\n",
	       htab_nr_mallocs - mallocs);
	htab_destroy(h);
	return 0;
}
