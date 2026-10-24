// SPDX-License-Identifier: MIT
/*
 * idalloc.h - two ID allocators built on bitmaps.
 *
 * Both hand out the lowest free ID in [min, max], as the kernel's IDA does.
 *
 *  - The flat allocator is one bitmap and find_next_zero_bit(): simple and
 *    compact, but finding a free ID may scan the whole map.
 *  - The hierarchical allocator puts a summary above the bitmap: one bit
 *    per word of the level below, set when that word is full.  Three
 *    levels of 64-bit words cover 64 * 64 * 64 = 262,144 IDs, and a search
 *    reads a few words per level instead of the whole map.  The IDA gets
 *    the same effect from the XArray's per-node "free" marks.
 *
 * What it leaves out on purpose: locking, growing on demand (the IDA
 * allocates 1024-bit chunks only when IDs in their range are used), and
 * cyclic allocation (used for PIDs).  words_read counts the words a
 * search touches, for the demo.
 */
#ifndef CH04_IDALLOC_H
#define CH04_IDALLOC_H

#include <errno.h>
#include "bitmap.h"

#define HB_LEVELS	3
#define HB_BITS		(1UL << (6 * HB_LEVELS))	/* 262,144 IDs */

/* Bits at level l (level 0 is the top word, level HB_LEVELS - 1 the leaves). */
static inline unsigned long level_bits(int l)
{
	return 1UL << (6 * (l + 1));
}

/* ------------------------------------------------------------------ flat */

struct flat_ids {
	DECLARE_BITMAP(used, HB_BITS);
	unsigned long words_read;
};

static inline long flat_alloc_range(struct flat_ids *f, unsigned long min,
				    unsigned long max)
{
	unsigned long id = find_next_zero_bit(f->used, HB_BITS, min);

	f->words_read += BIT_WORD(id < HB_BITS ? id : HB_BITS - 1) - BIT_WORD(min) + 1;
	if (id >= HB_BITS || id > max)
		return -ENOSPC;
	__set_bit(id, f->used);
	return id;
}

static inline void flat_free(struct flat_ids *f, unsigned long id)
{
	__clear_bit(id, f->used);
}

/* ---------------------------------------------------------- hierarchical */

struct hier_ids {
	/* full[l]: bit b set means word b of level l + 1 is full;
	 * at the leaf level, bit b set means ID b is in use. */
	unsigned long *full[HB_LEVELS];
	unsigned long store[1 + 64 + 64 * 64];
	unsigned long words_read;
};

static inline void hier_init(struct hier_ids *h)
{
	unsigned long *p = h->store;

	memset(h, 0, sizeof(*h));
	for (int l = 0; l < HB_LEVELS; l++) {
		h->full[l] = p;
		p += BITS_TO_LONGS(level_bits(l));
	}
}

/* Mark id used; a word that becomes full is marked in its parent. */
static inline void hier_set(struct hier_ids *h, unsigned long id)
{
	for (int l = HB_LEVELS - 1; l >= 0; l--) {
		unsigned long *w = &h->full[l][BIT_WORD(id)];

		*w |= BIT_MASK(id);
		if (*w != ~0UL)
			break;			/* still room: parents unchanged */
		id = BIT_WORD(id);		/* full: tell the parent */
	}
}

/* Mark id free; a word that was full is unmarked in its parent. */
static inline void hier_clear(struct hier_ids *h, unsigned long id)
{
	for (int l = HB_LEVELS - 1; l >= 0; l--) {
		unsigned long *w = &h->full[l][BIT_WORD(id)];
		bool was_full = *w == ~0UL;

		*w &= ~BIT_MASK(id);
		if (!was_full)
			break;			/* the parent never saw it as full */
		id = BIT_WORD(id);
	}
}

/* Lowest clear leaf bit at or after start, or HB_BITS. */
static inline unsigned long hier_next_zero(struct hier_ids *h, unsigned long start)
{
	unsigned long pos = start;
	int l = HB_LEVELS - 1;

	/* Climb until a word has a clear bit at or after pos. */
	for (;;) {
		unsigned long w;

		if (pos >= level_bits(l))
			return HB_BITS;
		w = h->full[l][BIT_WORD(pos)] | ~BITMAP_FIRST_WORD_MASK(pos);
		h->words_read++;
		if (w != ~0UL) {
			pos = BIT_WORD(pos) * BITS_PER_LONG + ffz(w);
			break;
		}
		if (l == 0)
			return HB_BITS;
		pos = BIT_WORD(pos) + 1;	/* next word, as a bit of the parent */
		l--;
	}
	/* Descend: a clear bit promises a clear bit in the child word. */
	while (l < HB_LEVELS - 1) {
		l++;
		pos = pos * BITS_PER_LONG + ffz(h->full[l][pos]);
		h->words_read++;
	}
	return pos;
}

static inline long hier_alloc_range(struct hier_ids *h, unsigned long min,
				    unsigned long max)
{
	unsigned long id = hier_next_zero(h, min);

	if (id >= HB_BITS || id > max)
		return -ENOSPC;
	hier_set(h, id);
	return id;
}

static inline void hier_free(struct hier_ids *h, unsigned long id)
{
	hier_clear(h, id);
}

#endif /* CH04_IDALLOC_H */
