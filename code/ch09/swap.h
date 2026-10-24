// SPDX-License-Identifier: MIT
/*
 * swap.h - the built-in element swaps shared by sort.c and min_heap.h.
 *
 * A generic sort knows only the size of an element.  Swapping two elements
 * one byte at a time is slow; swapping them a 64-bit word at a time is fast,
 * but only legal when the element size (and, on machines without cheap
 * unaligned access, the base address) is a multiple of 8.  So the sort
 * looks at the size and base once, picks one of three loops, and uses it
 * for every swap.  The kernel does the same in lib/sort.c and
 * include/linux/min_heap.h.
 *
 * Differences from the kernel, on purpose:
 *   - The kernel encodes the choice in the swap function pointer itself
 *     (the small integers 0, 1, 2 can never be real function addresses).
 *     Here the choice is a separate enum, which is easier to read.
 *   - Word loads and stores go through memcpy(), which compilers turn into
 *     single instructions; the kernel casts pointers instead, which is fine
 *     because it is built with -fno-strict-aliasing.
 *   - The base address is always checked for alignment; the kernel skips
 *     that check on machines with efficient unaligned access.
 */
#ifndef SWAP_H
#define SWAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum swap_kind { SWAP_WORDS_64, SWAP_WORDS_32, SWAP_BYTES };

/* Can elements of @size at @base be moved in chunks of @align bytes? */
static inline bool is_aligned(const void *base, size_t size, size_t align)
{
	return ((size | (uintptr_t)base) & (align - 1)) == 0;
}

static inline enum swap_kind swap_kind_for(const void *base, size_t size)
{
	if (is_aligned(base, size, 8))
		return SWAP_WORDS_64;
	if (is_aligned(base, size, 4))
		return SWAP_WORDS_32;
	return SWAP_BYTES;
}

/* Exchange @n bytes at @a and @b, walking down from the end. */
static inline void swap_words_64(char *a, char *b, size_t n)
{
	do {
		uint64_t x, y;

		n -= 8;
		memcpy(&x, a + n, 8);
		memcpy(&y, b + n, 8);
		memcpy(a + n, &y, 8);
		memcpy(b + n, &x, 8);
	} while (n);
}

static inline void swap_words_32(char *a, char *b, size_t n)
{
	do {
		uint32_t x, y;

		n -= 4;
		memcpy(&x, a + n, 4);
		memcpy(&y, b + n, 4);
		memcpy(a + n, &y, 4);
		memcpy(b + n, &x, 4);
	} while (n);
}

static inline void swap_bytes(char *a, char *b, size_t n)
{
	do {
		char t = a[--n];

		a[n] = b[n];
		b[n] = t;
	} while (n);
}

static inline void swap_builtin(enum swap_kind kind, void *a, void *b,
				size_t size)
{
	if (kind == SWAP_WORDS_64)
		swap_words_64(a, b, size);
	else if (kind == SWAP_WORDS_32)
		swap_words_32(a, b, size);
	else
		swap_bytes(a, b, size);
}

#endif /* SWAP_H */
