// SPDX-License-Identifier: MIT
/*
 * test_textsearch.c - randomized tests for textsearch.h.
 *
 *   ./test_textsearch [iterations] [seed]
 *
 * Each round makes a random text over a small alphabet (so that
 * patterns repeat and overlap a lot), cuts it into random chunks of 1 to
 * 12 bytes, and lists all matches with KMP.  The model is a naive search
 * on the contiguous text with the same "resume after the match"
 * semantics.  Boyer-Moore-Horspool must report only real matches, and
 * must agree with the model when the text is one single chunk.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include "textsearch.h"

static unsigned long long rng_state;

static unsigned long rnd(void)
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return (unsigned long)(rng_state * 2685821657736338717ULL);
}

static unsigned int rnd_below(unsigned int n)
{
	return n ? rnd() % n : 0;
}

/* The model: first match at or after `from` in contiguous text. */
static unsigned int naive_find(const uint8_t *t, unsigned int n,
			       const uint8_t *p, unsigned int m,
			       unsigned int from)
{
	for (unsigned int i = from; i + m <= n; i++)
		if (!memcmp(t + i, p, m))
			return i;
	return TS_NOMATCH;
}

int main(int argc, char **argv)
{
	unsigned long iters = argc > 1 ? strtoul(argv[1], NULL, 0) : 100000;
	uint8_t text[400], pat[12];
	struct chunk ch[400];
	unsigned long matches = 0, straddling = 0, bm_missed = 0;

	rng_state = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	if (!rng_state)
		rng_state = 1;

	for (unsigned long it = 0; it < iters; it++) {
		unsigned int sigma = 2 + rnd_below(3);
		unsigned int n = rnd_below(sizeof(text));
		unsigned int m = 1 + rnd_below(sizeof(pat));

		for (unsigned int i = 0; i < n; i++)
			text[i] = 'a' + rnd_below(sigma);
		if (n >= m && rnd_below(2)) {	/* often take it from the text */
			memcpy(pat, text + rnd_below(n - m + 1), m);
		} else {
			for (unsigned int i = 0; i < m; i++)
				pat[i] = 'a' + rnd_below(sigma);
		}

		/* Cut the text into chunks of 1..12 bytes. */
		struct chunks src = { ch, 0 };
		bool boundary[400] = { false };	/* boundary[i]: chunk starts at i */

		for (unsigned int off = 0; off < n; ) {
			unsigned int len = 1 + rnd_below(12);

			if (len > n - off)
				len = n - off;
			ch[src.nr++] = (struct chunk){ text + off, len };
			boundary[off] = true;
			off += len;
		}
		struct chunks whole_src = { (struct chunk[]){ { text, n } },
					    n ? 1 : 0 };

		struct kmp *kmp = kmp_prepare(pat, m);
		struct bmh *bm = bmh_prepare(pat, m);
		struct ts_state ks = { 0 }, ms = { 0 }, bs = { 0 }, ws = { 0 };

		assert(kmp && bm);
		for (;;) {
			unsigned int want = naive_find(text, n, pat, m, ms.offset);
			unsigned int got = kmp_find(kmp, &ks, chunks_next, &src);

			assert(got == want);
			if (want == TS_NOMATCH)
				break;
			ms.offset = want + m;
			assert(ks.offset == ms.offset);
			matches++;
			for (unsigned int i = want + 1; i < want + m; i++)
				if (boundary[i]) {
					straddling++;
					break;
				}
		}

		/* BMH on chunks: every hit is real; on one chunk: exact. */
		for (unsigned int got;
		     (got = bmh_find(bm, &bs, chunks_next, &src)) != TS_NOMATCH; )
			assert(got + m <= n && !memcmp(text + got, pat, m));
		ms.offset = 0;
		for (;;) {
			unsigned int want = naive_find(text, n, pat, m, ms.offset);
			unsigned int got = bmh_find(bm, &ws, chunks_next, &whole_src);

			assert(got == want);
			if (want == TS_NOMATCH)
				break;
			ms.offset = want + m;
		}

		/* Count first matches that BMH misses because of chunking. */
		bs.offset = 0;
		if (naive_find(text, n, pat, m, 0) != TS_NOMATCH &&
		    bmh_find(bm, &bs, chunks_next, &src) == TS_NOMATCH)
			bm_missed++;

		free(kmp);
		free(bm);
	}
	printf("ok: %lu rounds, %lu matches (%lu straddle a chunk boundary); "
	       "BMH missed every match in %lu texts\n",
	       iters, matches, straddling, bm_missed);
	return 0;
}
