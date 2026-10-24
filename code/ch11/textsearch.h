// SPDX-License-Identifier: MIT
/*
 * textsearch.h - a teaching version of the kernel's textsearch idea:
 * one search interface, several algorithms, and data that arrives in
 * blocks through a callback instead of as one contiguous buffer.
 *
 * The caller supplies get_next_block(consumed, &data, ctx).  It must
 * return a pointer to the data at byte offset `consumed` and the number
 * of bytes available there (0 at the end).  Blocks can have any size,
 * including 1 byte, exactly as with socket buffers in the kernel.
 *
 * Two algorithms are provided:
 *   - KMP, which carries its whole state (one integer) from block to
 *     block and therefore finds matches that straddle a block boundary;
 *   - a Boyer-Moore-Horspool search (bad-character rule only), which,
 *     like the kernel's Boyer-Moore, looks at each block on its own and
 *     misses straddling matches.
 *
 * Left out: algorithm registration and module loading, case-insensitive
 * search, the kernel's finite-state matcher, and the good-suffix rule of
 * full Boyer-Moore.
 */
#ifndef TEXTSEARCH_H
#define TEXTSEARCH_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TS_NOMATCH	UINT32_MAX

typedef unsigned int (*next_block_fn)(unsigned int consumed,
				      const uint8_t **data, void *ctx);

struct ts_state {
	unsigned int offset;		/* where the next search starts */
};

/* ------------------------------------------------------------------ KMP */

struct kmp {
	unsigned int len;
	uint8_t *pattern;
	unsigned int prefix[];		/* prefix[q]: see compute_prefix() */
};

/*
 * prefix[q] is the length of the longest proper prefix of
 * pattern[0..q] that is also a suffix of it.
 */
static void compute_prefix(const uint8_t *pat, unsigned int len,
			   unsigned int *prefix)
{
	unsigned int k = 0;

	prefix[0] = 0;
	for (unsigned int q = 1; q < len; q++) {
		while (k > 0 && pat[k] != pat[q])
			k = prefix[k - 1];	/* fall back to a shorter border */
		if (pat[k] == pat[q])
			k++;
		prefix[q] = k;
	}
}

static struct kmp *kmp_prepare(const void *pattern, unsigned int len)
{
	struct kmp *kmp;

	if (len == 0)
		return NULL;
	kmp = malloc(sizeof(*kmp) + len * sizeof(unsigned int) + len);
	if (!kmp)
		return NULL;
	kmp->len = len;
	kmp->pattern = (uint8_t *)&kmp->prefix[len];
	memcpy(kmp->pattern, pattern, len);
	compute_prefix(kmp->pattern, len, kmp->prefix);
	return kmp;
}

/*
 * Find the next match at or after state->offset.  Returns its offset,
 * or TS_NOMATCH.  On a match, state->offset moves just past it, so
 * repeated calls report non-overlapping matches, as in the kernel.
 */
static unsigned int kmp_find(const struct kmp *kmp, struct ts_state *state,
			     next_block_fn get_next_block, void *ctx)
{
	unsigned int q = 0;		/* pattern bytes matched so far */
	unsigned int consumed = state->offset;
	const uint8_t *text;
	unsigned int n;

	while ((n = get_next_block(consumed, &text, ctx)) != 0) {
		for (unsigned int i = 0; i < n; i++) {
			while (q > 0 && kmp->pattern[q] != text[i])
				q = kmp->prefix[q - 1];
			if (kmp->pattern[q] == text[i])
				q++;
			if (q == kmp->len) {
				state->offset = consumed + i + 1;
				return state->offset - kmp->len;
			}
		}
		consumed += n;		/* q carries over to the next block */
	}
	return TS_NOMATCH;
}

/* ----------------------------------------------------- Boyer-Moore-Horspool */

struct bmh {
	unsigned int len;
	unsigned int shift[256];
	uint8_t pattern[];
};

static struct bmh *bmh_prepare(const void *pattern, unsigned int len)
{
	struct bmh *bm;

	if (len == 0)
		return NULL;
	bm = malloc(sizeof(*bm) + len);
	if (!bm)
		return NULL;
	bm->len = len;
	memcpy(bm->pattern, pattern, len);
	for (int c = 0; c < 256; c++)
		bm->shift[c] = len;
	for (unsigned int i = 0; i + 1 < len; i++)
		bm->shift[bm->pattern[i]] = len - 1 - i;
	return bm;
}

/* Compares right to left inside one block; never looks across blocks. */
static unsigned int bmh_find(const struct bmh *bm, struct ts_state *state,
			     next_block_fn get_next_block, void *ctx)
{
	unsigned int consumed = state->offset;
	const uint8_t *text;
	unsigned int n;

	while ((n = get_next_block(consumed, &text, ctx)) != 0) {
		unsigned int end = bm->len - 1;	/* window's last byte */

		while (end < n) {
			unsigned int i = 0;

			while (i < bm->len &&
			       text[end - i] == bm->pattern[bm->len - 1 - i])
				i++;
			if (i == bm->len) {
				unsigned int pos = consumed + end + 1 - bm->len;

				state->offset = pos + bm->len;
				return pos;
			}
			end += bm->shift[text[end]];
		}
		consumed += n;
	}
	return TS_NOMATCH;
}

/* ------------------------------------------- a source made of chunks */

struct chunk {
	const uint8_t *data;
	unsigned int len;
};

struct chunks {
	const struct chunk *c;
	unsigned int nr;
};

/*
 * Like the kernel's skb sequential reader: return the rest of the
 * chunk that contains byte `consumed` (the caller need not use it all).
 */
static unsigned int chunks_next(unsigned int consumed, const uint8_t **data,
				void *ctx)
{
	const struct chunks *s = ctx;
	unsigned int start = 0;

	for (unsigned int i = 0; i < s->nr; i++) {
		if (consumed < start + s->c[i].len) {
			*data = s->c[i].data + (consumed - start);
			return start + s->c[i].len - consumed;
		}
		start += s->c[i].len;
	}
	return 0;
}

#endif /* TEXTSEARCH_H */
