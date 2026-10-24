// SPDX-License-Identifier: MIT
/*
 * min_heap.h - a teaching version of the kernel's binary min-heap
 * (include/linux/min_heap.h).
 *
 * The heap is an array the caller provides, so pushing never allocates:
 * a full heap refuses the push.  Elements are compared and swapped through
 * callbacks; a NULL swp selects a built-in swap from swap.h.  Each heap
 * type is declared with DEFINE_MIN_HEAP(type, name), which gives the
 * macros below the element type, so they check what they are given.
 *
 * Sift-down is the bottom-up variant also used by sort.c: follow the
 * smaller child to a leaf, then climb back.  Note the asymmetry with
 * sort.c: here the climb stops at the first element the sifted element is
 * not less than, so a sifted element that equals everything sinks to the
 * leaf (Exercise 9.4 measures what that costs).
 *
 * What it leaves out on purpose: the kernel's preallocated in-struct
 * storage (MIN_HEAP_PREALLOCATED), its out-of-line copies of each
 * function, and its WARN_ONCE() on misuse; here misuse returns false.
 */
#ifndef MIN_HEAP_H
#define MIN_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "swap.h"

#define DEFINE_MIN_HEAP(_type, _name)					\
	struct _name {							\
		size_t nr;	/* elements in the heap */		\
		size_t size;	/* capacity of data[] */		\
		_type *data;						\
	}

struct min_heap_callbacks {
	bool (*less)(const void *lhs, const void *rhs, void *args);
	void (*swp)(void *lhs, void *rhs, void *args);	/* may be NULL */
};

struct heap_ctx {		/* everything the generic code needs */
	char *data;
	size_t elem;
	const struct min_heap_callbacks *cb;
	void *args;
	enum swap_kind kind;
};

static inline bool heap_less(const struct heap_ctx *h, size_t i, size_t j)
{
	return h->cb->less(h->data + i * h->elem, h->data + j * h->elem,
			   h->args);
}

static inline void heap_swap(const struct heap_ctx *h, size_t i, size_t j)
{
	void *a = h->data + i * h->elem, *b = h->data + j * h->elem;

	if (h->cb->swp)
		h->cb->swp(a, b, h->args);
	else
		swap_builtin(h->kind, a, b, h->elem);
}

/* Sift the element at @pos down into the heap of @nr elements. */
static inline void __min_heap_sift_down(const struct heap_ctx *h,
					size_t pos, size_t nr)
{
	size_t i = pos, j;

	for (;;) {			/* follow the smaller child down */
		size_t c = 2 * i + 1, d = c + 1;

		if (d < nr)
			i = heap_less(h, c, d) ? c : d;
		else {
			if (c < nr)
				i = c;
			break;
		}
	}
	while (i != pos && heap_less(h, pos, i))	/* climb back */
		i = (i - 1) / 2;
	for (j = i; i != pos; ) {	/* rotate pos into place */
		i = (i - 1) / 2;
		heap_swap(h, i, j);
	}
}

/* Move the element at @i up while it is less than its parent. */
static inline void __min_heap_sift_up(const struct heap_ctx *h, size_t i)
{
	while (i) {
		size_t p = (i - 1) / 2;

		if (heap_less(h, p, i))
			break;
		heap_swap(h, i, p);
		i = p;
	}
}

static inline void __min_heapify_all(const struct heap_ctx *h, size_t nr)
{
	for (size_t i = nr / 2; i-- > 0; )
		__min_heap_sift_down(h, i, nr);
}

static inline bool __min_heap_push(const struct heap_ctx *h, size_t *nr,
				   size_t size, const void *elem)
{
	if (*nr >= size)
		return false;		/* full: the caller sized the array */
	memcpy(h->data + *nr * h->elem, elem, h->elem);
	__min_heap_sift_up(h, (*nr)++);
	return true;
}

static inline bool __min_heap_pop(const struct heap_ctx *h, size_t *nr)
{
	if (*nr == 0)
		return false;
	--*nr;				/* the last element replaces the root */
	memcpy(h->data, h->data + *nr * h->elem, h->elem);
	__min_heap_sift_down(h, 0, *nr);
	return true;
}

/* Replace the minimum with @elem: one sift instead of two. */
static inline void __min_heap_pop_push(const struct heap_ctx *h, size_t nr,
				       const void *elem)
{
	memcpy(h->data, elem, h->elem);
	__min_heap_sift_down(h, 0, nr);
}

/* Remove the element at index @idx. */
static inline bool __min_heap_del(const struct heap_ctx *h, size_t *nr,
				  size_t idx)
{
	if (idx >= *nr)
		return false;
	if (idx == --*nr)
		return true;
	heap_swap(h, idx, *nr);		/* the last element takes its place */
	__min_heap_sift_up(h, idx);	/* and moves whichever way it must */
	__min_heap_sift_down(h, idx, *nr);
	return true;
}

#define __heap_ctx(_heap, _cb, _args) (&(const struct heap_ctx){	\
	.data = (char *)(_heap)->data, .elem = sizeof(*(_heap)->data),	\
	.cb = (_cb), .args = (_args),					\
	.kind = swap_kind_for((_heap)->data, sizeof(*(_heap)->data)) })

#define min_heap_init(_heap, _data, _size) do {				\
	(_heap)->nr = 0;						\
	(_heap)->size = (_size);					\
	(_heap)->data = (_data);					\
} while (0)

#define min_heap_peek(_heap)	((_heap)->nr ? (_heap)->data : NULL)
#define min_heap_full(_heap)	((_heap)->nr == (_heap)->size)

#define min_heapify_all(_heap, _cb, _args)				\
	__min_heapify_all(__heap_ctx(_heap, _cb, _args), (_heap)->nr)

#define min_heap_sift_down(_heap, _pos, _cb, _args)			\
	__min_heap_sift_down(__heap_ctx(_heap, _cb, _args), _pos, (_heap)->nr)

#define min_heap_push(_heap, _elem, _cb, _args) ({			\
	const typeof(*(_heap)->data) *__e = (_elem);			\
	__min_heap_push(__heap_ctx(_heap, _cb, _args), &(_heap)->nr,	\
			(_heap)->size, __e); })

#define min_heap_pop(_heap, _cb, _args)					\
	__min_heap_pop(__heap_ctx(_heap, _cb, _args), &(_heap)->nr)

#define min_heap_pop_push(_heap, _elem, _cb, _args) do {		\
	const typeof(*(_heap)->data) *__e = (_elem);			\
	__min_heap_pop_push(__heap_ctx(_heap, _cb, _args),		\
			    (_heap)->nr, __e);				\
} while (0)

#define min_heap_del(_heap, _idx, _cb, _args)				\
	__min_heap_del(__heap_ctx(_heap, _cb, _args), &(_heap)->nr, _idx)

#endif /* MIN_HEAP_H */
