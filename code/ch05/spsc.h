// SPDX-License-Identifier: MIT
/*
 * spsc.h - a single-producer/single-consumer ring buffer with C11 atomics.
 *
 * Follows the design of the kernel's kfifo (include/linux/kfifo.h and
 * lib/kfifo.c): a power-of-two array of fixed-size elements and two
 * free-running unsigned indices, `in` (next slot to fill) and `out` (next
 * slot to empty).  The indices are never wrapped by hand; they overflow
 * naturally, `in - out` is always the number of filled slots, and
 * `index & mask` is the slot.  Every slot can be used.
 *
 * Ordering follows Documentation/core-api/circular-buffers.rst: each side
 * reads the other side's index with acquire and publishes its own with
 * release.  (v6.18's kfifo has only write barriers; see the chapter.)
 *
 * One thread may call the producer functions (spsc_put, spsc_in,
 * spsc_avail) and one the consumer functions (spsc_get, spsc_out,
 * spsc_peek, spsc_len); the two may run at the same time.
 *
 * Deliberately left out: kfifo's type-generic macros, record fifos
 * (variable-length elements with a length prefix), DMA scatterlists,
 * copying to and from user space, and the spinlocked wrappers that allow
 * several producers or several consumers.
 */
#ifndef SPSC_H
#define SPSC_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/*
 * The two orderings the ring depends on, named after the kernel's
 * primitives.  stress_spsc.c is also built with -DSPSC_RELAXED, which
 * quietly drops them, to show what ThreadSanitizer then reports.
 */
#define load_relaxed(p)		atomic_load_explicit(p, memory_order_relaxed)
#ifndef SPSC_RELAXED
#define smp_load_acquire(p)	atomic_load_explicit(p, memory_order_acquire)
#define smp_store_release(p, v)	atomic_store_explicit(p, v, memory_order_release)
#else	/* deliberately broken */
#define smp_load_acquire(p)	atomic_load_explicit(p, memory_order_relaxed)
#define smp_store_release(p, v)	atomic_store_explicit(p, v, memory_order_relaxed)
#endif

struct spsc {
	_Atomic unsigned int in;	/* written only by the producer */
	_Atomic unsigned int out;	/* written only by the consumer */
	unsigned int mask;		/* size - 1, size a power of two */
	unsigned int esize;		/* bytes per element */
	unsigned char *data;
};

static inline bool is_power_of_2(unsigned int n)
{
	return n && !(n & (n - 1));
}

/*
 * size elements of esize bytes each; size must be a power of two >= 2.
 * (kfifo_alloc() rounds up instead.)  Both indices start at `start`,
 * which the tests set close to UINT_MAX so that the indices wrap.
 */
static inline int spsc_init_at(struct spsc *r, unsigned int size,
			       unsigned int esize, unsigned int start)
{
	if (size < 2 || !is_power_of_2(size) || !esize)
		return -1;
	r->data = calloc(size, esize);
	if (!r->data)
		return -1;
	r->mask = size - 1;
	r->esize = esize;
	atomic_init(&r->in, start);
	atomic_init(&r->out, start);
	return 0;
}

static inline int spsc_init(struct spsc *r, unsigned int size,
			    unsigned int esize)
{
	return spsc_init_at(r, size, esize, 0);
}

static inline void spsc_free(struct spsc *r)
{
	free(r->data);
	r->data = NULL;
}

static inline unsigned int spsc_size(const struct spsc *r)
{
	return r->mask + 1;
}

/* Producer: free slots.  The consumer may free more at any moment. */
static inline unsigned int spsc_avail(struct spsc *r)
{
	unsigned int in = load_relaxed(&r->in);
	unsigned int out = smp_load_acquire(&r->out);

	return spsc_size(r) - (in - out);
}

/* Consumer: filled slots.  The producer may fill more at any moment. */
static inline unsigned int spsc_len(struct spsc *r)
{
	unsigned int out = load_relaxed(&r->out);
	unsigned int in = smp_load_acquire(&r->in);

	return in - out;
}

/* Producer: add one element; false if the ring is full. */
static inline bool spsc_put(struct spsc *r, const void *elem)
{
	unsigned int in = load_relaxed(&r->in);		/* our own index */
	unsigned int out = smp_load_acquire(&r->out);	/* the consumer's */

	if (in - out == spsc_size(r))
		return false;				/* full */
	memcpy(r->data + (in & r->mask) * r->esize, elem, r->esize);
	/* Publish: the element's bytes become visible before the index. */
	smp_store_release(&r->in, in + 1);
	return true;
}

/* Consumer: remove one element; false if the ring is empty. */
static inline bool spsc_get(struct spsc *r, void *elem)
{
	unsigned int out = load_relaxed(&r->out);	/* our own index */
	unsigned int in = smp_load_acquire(&r->in);	/* the producer's */

	if (in == out)
		return false;				/* empty */
	memcpy(elem, r->data + (out & r->mask) * r->esize, r->esize);
	/* Hand the slot back only after we have finished reading it. */
	smp_store_release(&r->out, out + 1);
	return true;
}

/* Copy len elements into the ring starting at index off: at most two pieces. */
static inline void spsc_copy_in(struct spsc *r, const void *src,
				unsigned int len, unsigned int off)
{
	unsigned int size = spsc_size(r), es = r->esize;
	unsigned int l;

	off &= r->mask;
	l = len < size - off ? len : size - off;	/* up to the end of the array */
	memcpy(r->data + off * es, src, l * es);
	memcpy(r->data, (const unsigned char *)src + l * es, (len - l) * es);
}

static inline void spsc_copy_out(struct spsc *r, void *dst,
				 unsigned int len, unsigned int off)
{
	unsigned int size = spsc_size(r), es = r->esize;
	unsigned int l;

	off &= r->mask;
	l = len < size - off ? len : size - off;
	memcpy(dst, r->data + off * es, l * es);
	memcpy((unsigned char *)dst + l * es, r->data, (len - l) * es);
}

/* Producer: add up to len elements; returns how many were added. */
static inline unsigned int spsc_in(struct spsc *r, const void *buf,
				   unsigned int len)
{
	unsigned int in = load_relaxed(&r->in);
	unsigned int out = smp_load_acquire(&r->out);
	unsigned int unused = spsc_size(r) - (in - out);

	if (len > unused)
		len = unused;
	spsc_copy_in(r, buf, len, in);
	smp_store_release(&r->in, in + len);
	return len;
}

/* Consumer: copy up to len elements without removing them. */
static inline unsigned int spsc_peek(struct spsc *r, void *buf,
				     unsigned int len)
{
	unsigned int out = load_relaxed(&r->out);
	unsigned int in = smp_load_acquire(&r->in);

	if (len > in - out)
		len = in - out;
	spsc_copy_out(r, buf, len, out);
	return len;
}

/* Consumer: remove up to len elements; returns how many were removed. */
static inline unsigned int spsc_out(struct spsc *r, void *buf,
				    unsigned int len)
{
	unsigned int out = load_relaxed(&r->out);

	len = spsc_peek(r, buf, len);
	smp_store_release(&r->out, out + len);
	return len;
}

#endif /* SPSC_H */
