// SPDX-License-Identifier: MIT
/*
 * test_bitmap.c - randomized tests for bitmap.h, idalloc.h and cpumask.h.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -o test_bitmap test_bitmap.c
 *   ./test_bitmap [iterations] [seed]
 *
 * Every bitmap is mirrored in a plain array of bools (the "model").  The
 * unused tail bits of each bitmap's last word are filled with random
 * garbage, which no result may depend on.  Bitmaps are allocated at
 * exactly the size they need, so AddressSanitizer catches any read past
 * the last word.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "bitmap.h"
#include "idalloc.h"
#include "cpumask.h"

static unsigned long long rng_state;

static unsigned long rnd(void)
{
	/* xorshift64* */
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return (unsigned long)(rng_state * 2685821657736338717ULL);
}

static unsigned long rnd_below(unsigned long n)
{
	return n ? rnd() % n : 0;
}

/* A random word with a random density of set bits. */
static unsigned long rnd_word(void)
{
	switch (rnd_below(5)) {
	case 0: return 0;
	case 1: return ~0UL;
	case 2: return 1UL << rnd_below(64);
	case 3: return rnd() & rnd() & rnd();
	default: return rnd();
	}
}

/* ------------------------------------------------------------ word ops */

static void test_words(unsigned long iters)
{
	for (unsigned long i = 0; i < iters; i++) {
		unsigned long w = rnd_word();

		assert(hweight_long(w) == (unsigned)__builtin_popcountl(w));
		if (w) {
			assert(__ffs(w) == (unsigned)__builtin_ctzl(w));
			assert(__fls(w) == 63u - __builtin_clzl(w));
		}
		if (~w)
			assert(ffz(w) == (unsigned)__builtin_ctzl(~w));
		unsigned int x = (unsigned int)w;

		assert(fls(x) == (x ? 32 - __builtin_clz(x) : 0));
	}
}

/* --------------------------------------------------------- bitmap ops */

struct tb {
	unsigned long size, *map;
	bool *model;
};

static void tb_new(struct tb *t, unsigned long size)
{
	t->size = size;
	t->map = malloc(BITS_TO_LONGS(size) * sizeof(unsigned long));
	t->model = calloc(size, 1);
	assert(t->map && t->model);
	for (unsigned long k = 0; k < BITS_TO_LONGS(size); k++)
		t->map[k] = rnd_word();
	for (unsigned long b = 0; b < size; b++)
		t->model[b] = test_bit(b, t->map);
}

static void tb_free(struct tb *t)
{
	free(t->map);
	free(t->model);
}

/* Scramble the tail bits beyond size: they are "don't care". */
static void tb_garbage(struct tb *t)
{
	unsigned long last = BITS_TO_LONGS(t->size) - 1;
	unsigned long keep = BITMAP_LAST_WORD_MASK(t->size);

	t->map[last] = (t->map[last] & keep) | (rnd() & ~keep);
}

static unsigned long model_next(const bool *m, unsigned long size,
				unsigned long start, bool val)
{
	for (unsigned long b = start; b < size; b++)
		if (m[b] == val)
			return b;
	return size;
}

static void tb_check(const struct tb *t)
{
	unsigned long n = 0;

	for (unsigned long b = 0; b < t->size; b++) {
		assert(test_bit(b, t->map) == t->model[b]);
		n += t->model[b];
	}
	assert(bitmap_weight(t->map, t->size) == n);
	assert(bitmap_empty(t->map, t->size) == (n == 0));
	assert(bitmap_full(t->map, t->size) == (n == t->size));

	unsigned long last = t->size;

	for (unsigned long b = t->size; b-- > 0;)
		if (t->model[b]) {
			last = b;
			break;
		}
	assert(find_last_bit(t->map, t->size) == last);

	for (int k = 0; k < 8; k++) {
		unsigned long s = rnd_below(t->size + 70);

		assert(find_next_bit(t->map, t->size, s) ==
		       (s < t->size ? model_next(t->model, t->size, s, true) : t->size));
		assert(find_next_zero_bit(t->map, t->size, s) ==
		       (s < t->size ? model_next(t->model, t->size, s, false) : t->size));

		unsigned long nth = rnd_below(n + 3), want = t->size, seen = 0;

		for (unsigned long b = 0; b < t->size; b++)
			if (t->model[b] && seen++ == nth) {
				want = b;
				break;
			}
		assert(find_nth_bit(t->map, t->size, nth) == want);
	}

	unsigned long b, cnt = 0;

	for_each_set_bit(b, t->map, t->size) {
		assert(t->model[b]);
		cnt++;
	}
	assert(cnt == n);
	cnt = 0;
	for_each_clear_bit(b, t->map, t->size) {
		assert(!t->model[b]);
		cnt++;
	}
	assert(cnt == t->size - n);
}

static unsigned long model_zero_area(const bool *m, unsigned long size,
				     unsigned long start, unsigned long nr,
				     unsigned long align)
{
	for (unsigned long i = (start + align) & ~align; i + nr <= size; i += align + 1) {
		unsigned long j;

		for (j = 0; j < nr && !m[i + j]; j++)
			;
		if (j == nr)
			return i;
	}
	return size;
}

static void test_bitmaps(unsigned long iters)
{
	static const unsigned long aligns[] = { 0, 1, 3, 7, 63 };

	for (unsigned long it = 0; it < iters; it++) {
		struct tb t, u;
		unsigned long size = 1 + (rnd_below(4) ? rnd_below(300) : rnd_below(3000));

		tb_new(&t, size);
		tb_new(&u, size);
		for (int step = 0; step < 20; step++) {
			unsigned long b = rnd_below(size), len, s;
			bool old = t.model[b];

			switch (rnd_below(10)) {
			case 0: __set_bit(b, t.map); t.model[b] = true; break;
			case 1: __clear_bit(b, t.map); t.model[b] = false; break;
			case 2: __change_bit(b, t.map); t.model[b] = !old; break;
			case 3: set_bit(b, t.map); t.model[b] = true; break;
			case 4: clear_bit(b, t.map); t.model[b] = false; break;
			case 5: change_bit(b, t.map); t.model[b] = !old; break;
			case 6:
				assert(test_and_set_bit(b, t.map) == old);
				t.model[b] = true;
				break;
			case 7:
				assert(test_and_clear_bit(b, t.map) == old);
				t.model[b] = false;
				break;
			case 8:
				s = rnd_below(size);
				len = rnd_below(size - s + 1);
				bitmap_set(t.map, s, len);
				for (unsigned long i = s; i < s + len; i++)
					t.model[i] = true;
				break;
			default:
				s = rnd_below(size);
				len = rnd_below(size - s + 1);
				bitmap_clear(t.map, s, len);
				for (unsigned long i = s; i < s + len; i++)
					t.model[i] = false;
				break;
			}
			tb_garbage(&t);
			tb_check(&t);

			/* bit lock */
			if (!t.model[b]) {
				assert(!test_and_set_bit_lock(b, t.map));
				assert(test_and_set_bit_lock(b, t.map));
				clear_bit_unlock(b, t.map);
				assert(!test_bit(b, t.map));
			}

			/* two-bitmap operations */
			tb_garbage(&u);
			unsigned long s2 = rnd_below(size + 5), want = size;

			for (unsigned long i = s2; i < size; i++)
				if (t.model[i] && u.model[i]) {
					want = i;
					break;
				}
			assert(find_next_and_bit(t.map, u.map, size, s2) == want);

			unsigned long *d = malloc(BITS_TO_LONGS(size) * sizeof(unsigned long));
			bool any = false;

			assert(d);
			bool r = bitmap_and(d, t.map, u.map, size);

			for (unsigned long i = 0; i < size; i++) {
				assert(test_bit(i, d) == (t.model[i] && u.model[i]));
				any |= t.model[i] && u.model[i];
			}
			assert(r == any);
			bitmap_or(d, t.map, u.map, size);
			for (unsigned long i = 0; i < size; i++)
				assert(test_bit(i, d) == (t.model[i] || u.model[i]));
			free(d);

			/* zero-area search */
			unsigned long al = aligns[rnd_below(5)];
			unsigned long nr = 1 + rnd_below(rnd_below(2) ? 4 : 40);
			unsigned long st = rnd_below(size);

			assert(bitmap_find_next_zero_area(t.map, size, st, nr, al) ==
			       model_zero_area(t.model, size, st, nr, al));
		}
		tb_free(&t);
		tb_free(&u);
	}
}

/* -------------------------------------------------------- ID allocators */

static bool used[HB_BITS];
static struct flat_ids flat;
static struct hier_ids hier;

static long model_alloc(unsigned long min, unsigned long max)
{
	const bool *p;

	if (min >= HB_BITS)
		return -ENOSPC;
	p = memchr(used + min, 0, HB_BITS - min);
	if (!p || (unsigned long)(p - used) > max)
		return -ENOSPC;
	used[p - used] = true;
	return p - used;
}

/* Every summary bit must say exactly whether its child word is full. */
static void check_summary(void)
{
	for (int l = 0; l < HB_LEVELS - 1; l++)
		for (unsigned long b = 0; b < level_bits(l); b++)
			assert(test_bit(b, hier.full[l]) ==
			       (hier.full[l + 1][b] == ~0UL));
	for (unsigned long id = 0; id < HB_BITS; id++)
		assert(test_bit(id, hier.full[HB_LEVELS - 1]) == used[id] &&
		       test_bit(id, flat.used) == used[id]);
}

static long one_alloc(unsigned long min, unsigned long max)
{
	long want = model_alloc(min, max);

	assert(flat_alloc_range(&flat, min, max) == want);
	assert(hier_alloc_range(&hier, min, max) == want);
	return want;
}

static void one_free(unsigned long id)
{
	used[id] = false;
	flat_free(&flat, id);
	hier_free(&hier, id);
}

static unsigned long random_used(void)
{
	for (;;) {
		unsigned long id = rnd_below(HB_BITS);

		if (used[id])
			return id;
	}
}

static void test_ids(unsigned long iters)
{
	unsigned long nfree;

	hier_init(&hier);
	memset(&flat, 0, sizeof(flat));
	memset(used, 0, sizeof(used));

	/* Sparse phase: random windows over a mostly empty space. */
	for (unsigned long i = 0; i < iters; i++) {
		unsigned long min = rnd_below(HB_BITS);
		unsigned long max = min + rnd_below(rnd_below(2) ? 100 : HB_BITS);

		if (rnd_below(3))
			one_alloc(min, max);
		else
			one_free(rnd_below(HB_BITS));
	}
	check_summary();

	/*
	 * Fill everything; each allocation must take the lowest free ID.
	 * Everything below the last ID handed out is full, so starting the
	 * search there gives the same answer and keeps the model fast.
	 */
	for (long id = 0; id >= 0;)
		id = one_alloc(id, HB_BITS - 1);
	check_summary();
	one_alloc(0, HB_BITS - 1);		/* full: -ENOSPC */

	/* Dense phase: a few hundred free IDs scattered in a full space. */
	nfree = 0;
	for (unsigned long i = 0; i < iters; i++) {
		if (nfree < 300 || rnd_below(2)) {
			one_free(random_used());
			nfree++;
		} else {
			unsigned long min = rnd_below(HB_BITS);
			unsigned long max = rnd_below(4) ? HB_BITS - 1 : min + rnd_below(5000);

			if (one_alloc(min, max) >= 0)
				nfree--;
		}
		if (i % 5000 == 0)
			check_summary();
	}
	check_summary();
}

/* --------------------------------------------------------------- cpumask */

static void test_cpumask(unsigned long iters)
{
	for (unsigned long it = 0; it < iters; it++) {
		cpumask_var_t a, b, c;
		static bool ma[NR_CPUS], mb[NR_CPUS];

		nr_cpu_ids = 1 + rnd_below(rnd_below(2) ? 200 : NR_CPUS);
		assert(zalloc_cpumask_var(&a) && zalloc_cpumask_var(&b) &&
		       zalloc_cpumask_var(&c));
		memset(ma, 0, sizeof(ma));
		memset(mb, 0, sizeof(mb));
		for (int k = 0; k < 30; k++) {
			unsigned int cpu = rnd_below(nr_cpu_ids);

			if (rnd_below(3)) {
				cpumask_set_cpu(cpu, a);
				ma[cpu] = true;
			} else {
				cpumask_clear_cpu(cpu, a);
				ma[cpu] = false;
			}
			cpu = rnd_below(nr_cpu_ids);
			cpumask_set_cpu(cpu, b);
			mb[cpu] = true;
		}
		unsigned int cpu, n = 0, nboth = 0, first = nr_cpu_ids;

		for (cpu = 0; cpu < nr_cpu_ids; cpu++) {
			n += ma[cpu];
			if (ma[cpu] && mb[cpu]) {
				nboth++;
				if (first == nr_cpu_ids)
					first = cpu;
			}
			assert(cpumask_test_cpu(cpu, a) == ma[cpu]);
		}
		assert(cpumask_weight(a) == n);
		assert(cpumask_first_and(a, b) == first);
		assert(cpumask_and(c, a, b) == (nboth > 0));
		assert(cpumask_weight(c) == nboth);
		cpumask_copy(c, a);
		for_each_cpu(cpu, c)
			assert(ma[cpu]);
		cpu = rnd_below(nr_cpu_ids);
		assert(cpumask_next(cpu, a) ==
		       model_next(ma, nr_cpu_ids, cpu + 1, true));
		free_cpumask_var(a);
		free_cpumask_var(b);
		free_cpumask_var(c);
	}
}

int main(int argc, char **argv)
{
	unsigned long iters = argc > 1 ? strtoul(argv[1], NULL, 0) : 20000;

	rng_state = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	rng_state = rng_state * 0x9E3779B97F4A7C15ULL + 1;

	test_words(iters * 10);
	test_bitmaps(iters / 10);
	test_ids(iters * 5);
	test_cpumask(iters / 10);
	printf("ok: %lu iterations, all checks passed\n", iters);
	return 0;
}
