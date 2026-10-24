// SPDX-License-Identifier: MIT
/*
 * test_lru.c - randomized test for lru.c against a slow, obviously correct
 * model: an array of (key, value, last-use time), searched linearly; the
 * victim is the entry with the oldest time.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -o test_lru test_lru.c
 *   ./test_lru [operations] [seed]
 */
#define LRU_NO_MAIN
#include "lru.c"

#include <assert.h>

#define CAP	16
#define KEYS	40

static struct { unsigned int key; int value; long used; } model[CAP];
static int mlen;
static long now;

static int m_find(unsigned int key)
{
	for (int i = 0; i < mlen; i++)
		if (model[i].key == key)
			return i;
	return -1;
}

static void m_put(unsigned int key, int value)
{
	int i = m_find(key);

	if (i < 0) {
		if (mlen < CAP) {
			i = mlen++;
		} else {			/* evict the oldest */
			i = 0;
			for (int j = 1; j < mlen; j++)
				if (model[j].used < model[i].used)
					i = j;
		}
		model[i].key = key;
	}
	model[i].value = value;
	model[i].used = ++now;
}

/* The cache's recency list must match the model sorted by time. */
static void check(struct lru_cache *c)
{
	struct lru_entry *e;
	long prev = now + 1;
	int n = 0;

	list_for_each_entry(e, &c->lru, lru) {
		int i = m_find(e->key);

		assert(i >= 0 && model[i].value == e->value);
		assert(model[i].used < prev);
		prev = model[i].used;
		n++;
	}
	assert(n == mlen);
}

int main(int argc, char **argv)
{
	long iters = argc > 1 ? atol(argv[1]) : 200000;
	struct lru_cache c;

	srand(argc > 2 ? atoi(argv[2]) : 1);
	assert(!lru_init(&c, CAP, 7));
	for (long it = 0; it < iters; it++) {
		unsigned int key = (unsigned int)(rand() % KEYS);
		int v, got, i = m_find(key);

		if (rand() % 2) {
			got = lru_get(&c, key, &v);
			assert(got == (i >= 0));
			if (got) {
				assert(v == model[i].value);
				model[i].used = ++now;
			}
		} else {
			v = rand();
			lru_put(&c, key, v);
			m_put(key, v);
		}
		check(&c);
	}
	printf("ok: %ld operations, %lu hits, %lu misses, %lu evictions\n",
	       iters, c.hits, c.misses, c.evictions);
	lru_destroy(&c);
	return 0;
}
