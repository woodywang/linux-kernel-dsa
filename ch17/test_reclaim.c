// SPDX-License-Identifier: MIT
/*
 * test_reclaim.c - model checks for reclaim.c.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -o test_reclaim \
 *      test_reclaim.c reclaim.c
 *   ./test_reclaim [rounds] [seed]
 *
 * For random small traces it checks that
 *   - exact LRU hits exactly when a brute-force model (a timestamp per
 *     page, evict the oldest) hits;
 *   - OPT's hit count equals a brute-force OPT that scans the rest of the
 *     trace for every resident page, and no policy beats it;
 *   - LRU has the inclusion property: every hit with C pages is also a
 *     hit with C + 1 pages;
 *   - every policy keeps its lists consistent after every access
 *     (sim_check), including MGLRU with extra agings mixed in;
 * and it checks shadow-entry packing and the wraparound of the distance.
 */
#include "reclaim.h"
#include "traces.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN	3000

static uint32_t trace[MAXN];

/* A random trace: a mix of loops, hot sets and noise over U pages. */
static long random_trace(int cap)
{
	long n = 1 + rng() % MAXN;
	uint32_t u = 1 + rng() % (3 * cap + 2);
	int mode = rng() % 4;

	for (long i = 0; i < n; i++) {
		uint32_t pg;

		switch (mode) {
		case 0:
			pg = rng() % u;
			break;
		case 1:
			pg = i % u;
			break;
		case 2:
			pg = rng() % 2 ? rng() % (u / 4 + 1) : rng() % u;
			break;
		default:
			pg = (i / 64) % 2 ? i % u : rng() % (cap + 1);
		}
		trace[i] = pg | (rng() % 3 ? FD_ACCESS : 0);
	}
	return n;
}

/* Brute-force LRU: returns the hit/miss sequence in hit[]. */
static void brute_lru(int cap, long n, bool *hit)
{
	static long last[MAX_PAGES];
	static uint32_t res[MAXN];
	int nr = 0;

	for (long i = 0; i < n; i++) {
		uint32_t pg = acc_page(trace[i]);
		int k;

		for (k = 0; k < nr && res[k] != pg; k++)
			;
		hit[i] = k < nr;
		if (!hit[i]) {
			if (nr == cap) {
				int v = 0;

				for (int j = 1; j < nr; j++)
					if (last[res[j]] < last[res[v]])
						v = j;
				res[v] = pg;
			} else {
				res[nr++] = pg;
			}
		}
		last[pg] = i;
	}
}

/* Brute-force OPT: evict the resident page used furthest in the future. */
static long brute_opt(int cap, long n)
{
	static uint32_t res[MAXN];
	int nr = 0;
	long hits = 0;

	for (long i = 0; i < n; i++) {
		uint32_t pg = acc_page(trace[i]);
		int k;

		for (k = 0; k < nr && res[k] != pg; k++)
			;
		if (k < nr) {
			hits++;
			continue;
		}
		if (nr < cap) {
			res[nr++] = pg;
			continue;
		}
		int v = 0;
		long far = -1;

		for (int j = 0; j < nr; j++) {
			long nx;

			for (nx = i + 1; nx < n && acc_page(trace[nx]) != res[j]; nx++)
				;
			if (nx > far) {
				far = nx;
				v = j;
			}
		}
		res[v] = pg;
	}
	return hits;
}

static void test_shadow(void)
{
	for (int i = 0; i < 100000; i++) {
		int memcg = rng() & 0xffff, nid = rng() & 63, m2, n2;
		unsigned long ev = ((unsigned long)rng() << 32 | rng()) &
				   (~0UL >> 24), ev2;

		if (i % 2)		/* close to the top: the clock will wrap */
			ev = (~0UL >> 24) - rng() % 1000;
		bool ws = rng() & 1, ws2;
		unsigned long sh = pack_shadow(memcg, nid, ev, ws);

		assert(sh & 1);			/* a value entry, not a pointer */
		unpack_shadow(sh, &m2, &n2, &ev2, &ws2);
		assert(m2 == memcg && n2 == nid && ev2 == ev && ws2 == ws);

		/* the clock may wrap between eviction and refault */
		unsigned long d = rng() % 100000;
		unsigned long now = (ev + d) & (~0UL >> 24);

		assert(refault_distance(now, ev) == d);
	}
}

int main(int argc, char **argv)
{
	long rounds = argc > 1 ? atol(argv[1]) : 400;
	unsigned int seed = argc > 2 ? atoi(argv[2]) : 1;
	static bool hit_a[MAXN], hit_b[MAXN];
	long checked = 0;

	rng_seed(seed);
	test_shadow();

	for (long r = 0; r < rounds; r++) {
		int cap = 1 + rng() % 40;
		long n = random_trace(cap), best = 0, opt = 0;

		/* exact LRU against the brute-force model, access by access */
		brute_lru(cap, n, hit_a);
		struct sim *s = sim_new(POL_LRU, cap);

		for (long i = 0; i < n; i++)
			assert(sim_access(s, trace, n, i) == hit_a[i]);
		sim_free(s);

		/* inclusion: a bigger LRU cache hits wherever a smaller one does */
		brute_lru(cap + 1, n, hit_b);
		for (long i = 0; i < n; i++)
			assert(!hit_a[i] || hit_b[i]);

		/* every policy: invariants after every access */
		for (int p = 0; p < NR_POLICIES; p++) {
			s = sim_new(p, cap);
			if (p == POL_OPT)
				sim_set_trace(s, trace, n);
			for (long i = 0; i < n; i++) {
				sim_access(s, trace, n, i);
				if (p == POL_MGLRU && rng() % 50 == 0)
					sim_age(s);	/* extra agings */
				sim_check(s);
				checked++;
			}
			const struct sim_stats *st = sim_stats(s);

			assert(st->hits + st->misses == n);
			if (p != POL_OPT && st->hits > best)
				best = st->hits;
			if (p == POL_OPT)
				opt = st->hits;
			sim_free(s);
		}

		/* OPT is optimal and matches the brute-force version */
		assert(opt == brute_opt(cap, n));
		assert(opt >= best);
	}

	/* the shift trace: shadows must rescue the second working set */
	{
		static uint32_t t[200000];
		struct sim_stats a, b;
		long n = trace_shift(t, 200000, 1024);

		sim_run(POL_2LIST, 1024, t, n, &a);
		sim_run(POL_2LIST_WS, 1024, t, n, &b);
		assert(b.refault_activations > 0 && b.hits > a.hits);
	}

	printf("ok: %ld rounds, %ld invariant checks, seed %u\n",
	       rounds, checked, seed);
	return 0;
}
