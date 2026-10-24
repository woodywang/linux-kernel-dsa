// SPDX-License-Identifier: MIT
/*
 * bitmap.h - a small teaching version of the kernel's bitmaps, bit search,
 * and single-bit operations.
 *
 * A bitmap is an array of unsigned longs; bit n lives in word n / 64 at
 * position n % 64.  Every search works a word at a time: skip words that
 * cannot contain an answer, then find the answer inside one word with a
 * word-level primitive (__ffs, __fls, hweight_long).
 *
 * What it shows:   the word layout and masks, __ffs/__fls/ffz/hweight_long in
 *                  portable C, one search loop shared by the find_next_*
 *                  family, find_last_bit, find_nth_bit, range set/clear,
 *                  whole-bitmap operations, the zero-area search used by
 *                  allocators, and atomic versus non-atomic bit operations
 *                  (with GCC/Clang __atomic builtins).
 * What it leaves out on purpose: the compile-time single-word fast paths,
 *                  architecture instructions (the kernel uses e.g. tzcnt and
 *                  popcnt on x86), big-endian and little-endian-on-disk
 *                  variants, parsing and printing, and 32-bit longs (it
 *                  assumes a 64-bit unsigned long).
 */
#ifndef CH04_BITMAP_H
#define CH04_BITMAP_H

#include <limits.h>
#include <stdbool.h>
#include <string.h>

_Static_assert(sizeof(unsigned long) == 8, "the teaching code assumes 64-bit longs");

#define BITS_PER_LONG		64UL
#define BITS_TO_LONGS(nr)	(((nr) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define DECLARE_BITMAP(name, bits)	unsigned long name[BITS_TO_LONGS(bits)]

#define BIT_WORD(nr)		((nr) / BITS_PER_LONG)
#define BIT_MASK(nr)		(1UL << ((nr) % BITS_PER_LONG))

/* Bits at or above start's position in its word; bits below nbits in the last word. */
#define BITMAP_FIRST_WORD_MASK(start)	(~0UL << ((start) & (BITS_PER_LONG - 1)))
#define BITMAP_LAST_WORD_MASK(nbits)	(~0UL >> (-(nbits) & (BITS_PER_LONG - 1)))

static inline unsigned long min_ul(unsigned long a, unsigned long b)
{
	return a < b ? a : b;
}

/* ---------------------------------------------------------------------
 * Word-level primitives.  __ffs and __fls are undefined for 0, like the
 * kernel's; ffs and fls count from 1 and return 0 for 0.
 */

/* Index of the lowest set bit: a binary search, halving the window. */
static inline unsigned int __ffs(unsigned long word)
{
	unsigned int num = 0;

	if (!(word & 0xffffffffUL)) {
		num += 32;
		word >>= 32;
	}
	if (!(word & 0xffffUL)) {
		num += 16;
		word >>= 16;
	}
	if (!(word & 0xffUL)) {
		num += 8;
		word >>= 8;
	}
	if (!(word & 0xfUL)) {
		num += 4;
		word >>= 4;
	}
	if (!(word & 0x3UL)) {
		num += 2;
		word >>= 2;
	}
	if (!(word & 0x1UL))
		num += 1;
	return num;
}

/* Index of the highest set bit, by the same halving from the top. */
static inline unsigned int __fls(unsigned long word)
{
	unsigned int num = BITS_PER_LONG - 1;

	if (!(word & (~0UL << 32))) {
		num -= 32;
		word <<= 32;
	}
	if (!(word & (~0UL << 48))) {
		num -= 16;
		word <<= 16;
	}
	if (!(word & (~0UL << 56))) {
		num -= 8;
		word <<= 8;
	}
	if (!(word & (~0UL << 60))) {
		num -= 4;
		word <<= 4;
	}
	if (!(word & (~0UL << 62))) {
		num -= 2;
		word <<= 2;
	}
	if (!(word & (~0UL << 63)))
		num -= 1;
	return num;
}

/* Index of the lowest clear bit; undefined for ~0UL. */
static inline unsigned int ffz(unsigned long word)
{
	return __ffs(~word);
}

static inline int fls(unsigned int x)
{
	return x ? (int)__fls(x) + 1 : 0;
}

/* Population count ("Hamming weight") in four steps, no loop. */
static inline unsigned int hweight_long(unsigned long w)
{
	w -= (w >> 1) & 0x5555555555555555UL;		/* 32 sums of 2 bits */
	w = (w & 0x3333333333333333UL) +
	    ((w >> 2) & 0x3333333333333333UL);		/* 16 sums of 4 bits */
	w = (w + (w >> 4)) & 0x0f0f0f0f0f0f0f0fUL;	/* 8 sums of 8 bits */
	return (w * 0x0101010101010101UL) >> 56;	/* add the 8 bytes */
}

/* ---------------------------------------------------------------------
 * Searching.  Every search returns size when there is no answer.
 *
 * One loop serves the whole find_next family.  FETCH is an expression
 * that yields word idx of the bitmap being searched: addr[idx] to find a
 * set bit, ~addr[idx] to find a clear one, a[idx] & b[idx] for a bit set
 * in both.
 */
#define FIND_NEXT_BIT(FETCH, size, start)				\
({									\
	unsigned long idx, tmp, sz = (size), st = (start), res = sz;	\
									\
	if (st < sz) {							\
		idx = st / BITS_PER_LONG;				\
		tmp = (FETCH) & BITMAP_FIRST_WORD_MASK(st);		\
		while (!tmp && (idx + 1) * BITS_PER_LONG < sz) {	\
			idx++;						\
			tmp = (FETCH);	/* skip a whole word */		\
		}							\
		if (tmp)	/* a hit in the tail is clamped to sz */ \
			res = min_ul(idx * BITS_PER_LONG + __ffs(tmp), sz); \
	}								\
	res;								\
})

static inline unsigned long find_next_bit(const unsigned long *addr,
					  unsigned long size, unsigned long start)
{
	return FIND_NEXT_BIT(addr[idx], size, start);
}

static inline unsigned long find_next_zero_bit(const unsigned long *addr,
					       unsigned long size, unsigned long start)
{
	return FIND_NEXT_BIT(~addr[idx], size, start);
}

static inline unsigned long find_next_and_bit(const unsigned long *a,
					      const unsigned long *b,
					      unsigned long size, unsigned long start)
{
	return FIND_NEXT_BIT(a[idx] & b[idx], size, start);
}

static inline unsigned long find_first_bit(const unsigned long *addr, unsigned long size)
{
	return find_next_bit(addr, size, 0);
}

static inline unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size)
{
	return find_next_zero_bit(addr, size, 0);
}

/* Scan backwards from the last word, ignoring the tail beyond size. */
static inline unsigned long find_last_bit(const unsigned long *addr, unsigned long size)
{
	unsigned long idx = BITS_TO_LONGS(size);
	unsigned long mask = BITMAP_LAST_WORD_MASK(size);

	while (idx-- > 0) {
		unsigned long w = addr[idx] & mask;

		if (w)
			return idx * BITS_PER_LONG + __fls(w);
		mask = ~0UL;
	}
	return size;
}

/* The n-th set bit (counting from 0): skip whole words by their weight. */
static inline unsigned long find_nth_bit(const unsigned long *addr,
					 unsigned long size, unsigned long n)
{
	for (unsigned long idx = 0; idx * BITS_PER_LONG < size; idx++) {
		unsigned long w = addr[idx];
		unsigned int cnt;

		if ((idx + 1) * BITS_PER_LONG > size)
			w &= BITMAP_LAST_WORD_MASK(size);	/* ignore the tail */
		cnt = hweight_long(w);
		if (n < cnt) {
			while (n--)
				w &= w - 1;		/* clear the lowest set bit */
			return idx * BITS_PER_LONG + __ffs(w);
		}
		n -= cnt;
	}
	return size;
}

#define for_each_set_bit(bit, addr, size)				\
	for ((bit) = find_next_bit((addr), (size), 0);			\
	     (bit) < (size);						\
	     (bit) = find_next_bit((addr), (size), (bit) + 1))

#define for_each_clear_bit(bit, addr, size)				\
	for ((bit) = find_next_zero_bit((addr), (size), 0);		\
	     (bit) < (size);						\
	     (bit) = find_next_zero_bit((addr), (size), (bit) + 1))

/* ---------------------------------------------------------------------
 * Whole-bitmap operations.  They work a word at a time and are not
 * atomic.  Bits beyond nbits in the last word are "don't care": results
 * never depend on them.
 */
static inline void bitmap_zero(unsigned long *dst, unsigned long nbits)
{
	memset(dst, 0, BITS_TO_LONGS(nbits) * sizeof(unsigned long));
}

/* Returns true if the result has any bit set. */
static inline bool bitmap_and(unsigned long *dst, const unsigned long *a,
			      const unsigned long *b, unsigned long nbits)
{
	unsigned long k, n = BITS_TO_LONGS(nbits), any = 0;

	for (k = 0; k < n; k++)
		dst[k] = a[k] & b[k];
	for (k = 0; k + 1 < n; k++)
		any |= dst[k];
	if (n)
		any |= dst[n - 1] & BITMAP_LAST_WORD_MASK(nbits);
	return any != 0;
}

static inline void bitmap_or(unsigned long *dst, const unsigned long *a,
			     const unsigned long *b, unsigned long nbits)
{
	for (unsigned long k = 0; k < BITS_TO_LONGS(nbits); k++)
		dst[k] = a[k] | b[k];
}

static inline unsigned long bitmap_weight(const unsigned long *src, unsigned long nbits)
{
	unsigned long k, w = 0;

	for (k = 0; k < nbits / BITS_PER_LONG; k++)
		w += hweight_long(src[k]);
	if (nbits % BITS_PER_LONG)
		w += hweight_long(src[k] & BITMAP_LAST_WORD_MASK(nbits));
	return w;
}

static inline bool bitmap_empty(const unsigned long *src, unsigned long nbits)
{
	return find_first_bit(src, nbits) >= nbits;
}

static inline bool bitmap_full(const unsigned long *src, unsigned long nbits)
{
	return find_first_zero_bit(src, nbits) >= nbits;
}

/* Set bits [start, start + len): partial words at the ends, whole words between. */
static inline void bitmap_set(unsigned long *map, unsigned long start, unsigned long len)
{
	unsigned long end = start + len;

	while (start < end) {
		unsigned long idx = BIT_WORD(start);
		unsigned long mask = BITMAP_FIRST_WORD_MASK(start);

		if (end < (idx + 1) * BITS_PER_LONG)
			mask &= BITMAP_LAST_WORD_MASK(end);
		map[idx] |= mask;
		start = (idx + 1) * BITS_PER_LONG;
	}
}

static inline void bitmap_clear(unsigned long *map, unsigned long start, unsigned long len)
{
	unsigned long end = start + len;

	while (start < end) {
		unsigned long idx = BIT_WORD(start);
		unsigned long mask = BITMAP_FIRST_WORD_MASK(start);

		if (end < (idx + 1) * BITS_PER_LONG)
			mask &= BITMAP_LAST_WORD_MASK(end);
		map[idx] &= ~mask;
		start = (idx + 1) * BITS_PER_LONG;
	}
}

/*
 * Find nr consecutive clear bits starting at a multiple of align_mask + 1
 * (a power of two), at or after start.  Returns size if there is none.
 */
static inline unsigned long bitmap_find_next_zero_area(const unsigned long *map,
						       unsigned long size,
						       unsigned long start,
						       unsigned long nr,
						       unsigned long align_mask)
{
	for (;;) {
		unsigned long index = find_next_zero_bit(map, size, start);
		unsigned long end, busy;

		index = (index + align_mask) & ~align_mask;	/* round up */
		end = index + nr;
		if (index >= size || end > size)
			return size;
		busy = find_next_bit(map, end, index);
		if (busy >= end)
			return index;		/* [index, end) is all clear */
		start = busy + 1;		/* restart after the obstacle */
	}
}

/* ---------------------------------------------------------------------
 * Single-bit operations.  The "__" versions are plain read-modify-write
 * sequences: two CPUs changing different bits of one word can lose an
 * update.  The others are atomic.  Ordering follows the kernel's rules:
 * operations without a return value are unordered, operations that
 * return the old value are fully ordered (approximated here with
 * sequentially consistent atomics), and the _lock/_unlock pair has
 * acquire/release semantics.
 */
static inline bool test_bit(unsigned long nr, const unsigned long *addr)
{
	return (__atomic_load_n(&addr[BIT_WORD(nr)], __ATOMIC_RELAXED) >>
		(nr % BITS_PER_LONG)) & 1;
}

static inline void __set_bit(unsigned long nr, unsigned long *addr)
{
	addr[BIT_WORD(nr)] |= BIT_MASK(nr);	/* load, OR, store */
}

static inline void __clear_bit(unsigned long nr, unsigned long *addr)
{
	addr[BIT_WORD(nr)] &= ~BIT_MASK(nr);
}

static inline void __change_bit(unsigned long nr, unsigned long *addr)
{
	addr[BIT_WORD(nr)] ^= BIT_MASK(nr);
}

static inline void set_bit(unsigned long nr, unsigned long *addr)
{
	__atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_RELAXED);
}

static inline void clear_bit(unsigned long nr, unsigned long *addr)
{
	__atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_RELAXED);
}

static inline void change_bit(unsigned long nr, unsigned long *addr)
{
	__atomic_fetch_xor(&addr[BIT_WORD(nr)], BIT_MASK(nr), __ATOMIC_RELAXED);
}

static inline bool test_and_set_bit(unsigned long nr, unsigned long *addr)
{
	unsigned long old = __atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr),
					      __ATOMIC_SEQ_CST);

	return old & BIT_MASK(nr);
}

static inline bool test_and_clear_bit(unsigned long nr, unsigned long *addr)
{
	unsigned long old = __atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr),
					       __ATOMIC_SEQ_CST);

	return old & BIT_MASK(nr);
}

/* A one-bit lock: acquire ordering on success, release on unlock. */
static inline bool test_and_set_bit_lock(unsigned long nr, unsigned long *addr)
{
	if (test_bit(nr, addr))
		return true;		/* held: don't write to the line at all */
	return __atomic_fetch_or(&addr[BIT_WORD(nr)], BIT_MASK(nr),
				 __ATOMIC_ACQUIRE) & BIT_MASK(nr);
}

static inline void clear_bit_unlock(unsigned long nr, unsigned long *addr)
{
	__atomic_fetch_and(&addr[BIT_WORD(nr)], ~BIT_MASK(nr), __ATOMIC_RELEASE);
}

#endif /* CH04_BITMAP_H */
