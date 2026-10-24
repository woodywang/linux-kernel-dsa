// SPDX-License-Identifier: MIT
/*
 * htab.c - a preallocated hash map in the shape of the BPF hash map.
 * See htab.h for what it leaves out.
 */
#include "htab.h"
#include "jhash.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int htab_cpu;
unsigned long htab_nr_mallocs;

static void *counted_calloc(size_t n, size_t size)
{
	htab_nr_mallocs++;
	return calloc(n, size);
}

static uint32_t round_up8(uint32_t x)
{
	return (x + 7) & ~7u;
}

struct htab_elem *htab_elem_at(const struct htab *h, uint32_t i)
{
	return (struct htab_elem *)(h->elems + (size_t)i * h->elem_size);
}

static void *elem_value(const struct htab *h, struct htab_elem *l)
{
	return l->key + round_up8(h->key_size);
}

/* ---- per-CPU free lists ------------------------------------------- */

static void freelist_push(struct htab *h, struct htab_elem *l)
{
	/* Push onto this CPU's list. */
	l->free_next = h->freelist[htab_cpu];
	h->freelist[htab_cpu] = l;
}

static struct htab_elem *freelist_pop(struct htab *h)
{
	/* Try this CPU's list first, then the others in turn. */
	for (int i = 0; i < NR_CPUS; i++) {
		int cpu = (htab_cpu + i) % NR_CPUS;
		struct htab_elem *l = h->freelist[cpu];

		if (l) {
			h->freelist[cpu] = l->free_next;
			return l;
		}
	}
	return NULL;			/* every element is in use */
}

int htab_free_count(const struct htab *h, int cpu)
{
	int n = 0;

	for (struct htab_elem *l = h->freelist[cpu]; l; l = l->free_next)
		n++;
	return n;
}

/* ---- creation ------------------------------------------------------ */

struct htab *htab_create(uint32_t key_size, uint32_t value_size,
			 uint32_t max_entries, uint32_t seed)
{
	uint32_t total = max_entries + NR_CPUS;
	struct htab *h;

	if (!key_size || !value_size || !max_entries || max_entries > 1u << 24)
		return NULL;
	h = counted_calloc(1, sizeof(*h));
	if (!h)
		return NULL;
	h->key_size = key_size;
	h->value_size = value_size;
	h->max_entries = max_entries;
	h->hashrnd = seed;
	h->n_buckets = 1;
	while (h->n_buckets < max_entries)
		h->n_buckets <<= 1;
	h->elem_size = sizeof(struct htab_elem) + round_up8(key_size) +
		       round_up8(value_size);
	h->buckets = counted_calloc(h->n_buckets, sizeof(struct bucket));
	h->elems = counted_calloc(total, h->elem_size);
	if (!h->buckets || !h->elems) {
		htab_destroy(h);
		return NULL;
	}
	for (uint32_t i = 0; i < h->n_buckets; i++)
		h->buckets[i].first = nulls_marker(i);

	/* Give each CPU an equal share of consecutive elements ... */
	uint32_t idx = 0;

	for (int cpu = 0; cpu < NR_CPUS; cpu++) {
		uint32_t share = total / NR_CPUS + (cpu < (int)(total % NR_CPUS));

		for (uint32_t j = 0; j < share; j++) {
			struct htab_elem *l = htab_elem_at(h, idx++);

			l->free_next = h->freelist[cpu];
			h->freelist[cpu] = l;
		}
	}
	/* ... then take one spare per CPU, as the kernel does. */
	for (int cpu = 0; cpu < NR_CPUS; cpu++)
		h->extra[cpu] = freelist_pop(h);
	return h;
}

void htab_destroy(struct htab *h)
{
	if (!h)
		return;
	free(h->elems);
	free(h->buckets);
	free(h);
}

/* ---- lookup -------------------------------------------------------- */

static struct bucket *select_bucket(struct htab *h, uint32_t hash)
{
	return &h->buckets[hash & (h->n_buckets - 1)];
}

/*
 * The lock-free lookup.  An element can be deleted and reused in another
 * bucket while a reader stands on it, so a walk that ends at the wrong
 * nulls marker starts again.
 */
static struct htab_elem *lookup_nulls(struct htab *h, struct bucket *b,
				      uint32_t hash, const void *key)
{
	struct htab_elem *l;

again:
	for (l = b->first; !is_nulls(l); l = l->next)
		if (l->hash == hash && !memcmp(l->key, key, h->key_size))
			return l;
	if (nulls_value(l) != (hash & (h->n_buckets - 1)))
		goto again;
	return NULL;
}

void *htab_lookup(struct htab *h, const void *key)
{
	uint32_t hash = map_hash(key, h->key_size, h->hashrnd);
	struct htab_elem *l = lookup_nulls(h, select_bucket(h, hash), hash, key);

	return l ? elem_value(h, l) : NULL;
}

/* ---- update and delete ("under the bucket lock") ------------------- */

static void unlink_elem(struct bucket *b, struct htab_elem *victim)
{
	struct htab_elem **pp = &b->first;

	while (*pp != victim)
		pp = &(*pp)->next;
	*pp = victim->next;	/* victim->next is left as it was */
}

int htab_update(struct htab *h, const void *key, const void *value,
		int flags)
{
	uint32_t hash = map_hash(key, h->key_size, h->hashrnd);
	struct bucket *b = select_bucket(h, hash);
	struct htab_elem *l_old, *l_new;

	if (flags < BPF_ANY || flags > BPF_EXIST)
		return -EINVAL;
	/* lock the bucket */
	l_old = lookup_nulls(h, b, hash, key);
	if (l_old && flags == BPF_NOEXIST)
		return -EEXIST;
	if (!l_old && flags == BPF_EXIST)
		return -ENOENT;

	if (l_old) {
		/* Replace: use this CPU's spare; the old one becomes it. */
		l_new = h->extra[htab_cpu];
		h->extra[htab_cpu] = l_old;
	} else {
		l_new = freelist_pop(h);
		if (!l_new)
			return -E2BIG;	/* all max_entries elements in use */
	}
	memcpy(l_new->key, key, h->key_size);
	memcpy(elem_value(h, l_new), value, h->value_size);
	l_new->hash = hash;

	/* Publish at the head, so a reader finds it before the old one. */
	l_new->next = b->first;
	b->first = l_new;
	if (l_old)
		unlink_elem(b, l_old);
	/* unlock the bucket */
	return 0;
}

int htab_delete(struct htab *h, const void *key)
{
	uint32_t hash = map_hash(key, h->key_size, h->hashrnd);
	struct bucket *b = select_bucket(h, hash);
	struct htab_elem *l;

	/* lock the bucket */
	l = lookup_nulls(h, b, hash, key);
	if (!l)
		return -ENOENT;
	unlink_elem(b, l);
	/* unlock the bucket */
	freelist_push(h, l);	/* reusable at once: no grace period */
	return 0;
}
