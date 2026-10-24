// SPDX-License-Identifier: MIT
/*
 * ring_demo.c - free-running indices, masked indices, and why the size is
 * a power of two.
 *
 *   cc -O2 -Wall -o ring_demo ring_demo.c && ./ring_demo
 *
 * Part 1 drives an 8-slot spsc ring whose indices start just below
 * UINT_MAX, so they overflow in the middle of the run; `in - out` stays
 * correct throughout.  Part 2 fills a ring kept the way
 * include/linux/circ_buf.h keeps it, with indices wrapped to the size
 * after every step: equal indices must mean "empty", so it can never use
 * its last slot.  Part 3 shows what overflow does to a ring whose size is
 * not a power of two.
 */
#include <limits.h>
#include <stdio.h>
#include "spsc.h"

/* The two macros from include/linux/circ_buf.h that part 2 needs. */
#define CIRC_CNT(head, tail, size)	(((head) - (tail)) & ((size) - 1))
#define CIRC_SPACE(head, tail, size)	CIRC_CNT((tail), ((head) + 1), (size))

static void show(struct spsc *r, const char *op, unsigned int n,
		 const char *verb)
{
	unsigned int in = load_relaxed(&r->in), out = load_relaxed(&r->out);

	printf("%-7s -> %u %-8s in=%-10u out=%-10u in-out=%u%s\n", op, n, verb,
	       in, out, in - out, in - out == spsc_size(r) ? " (full)" : "");
}

int main(void)
{
	char buf[8] = "abcdefgh";
	struct spsc r;
	unsigned int start = UINT_MAX - 3;

	/* Part 1: indices that run free. */
	spsc_init_at(&r, 8, 1, start);
	printf("8-slot ring, both indices start at %u (slot %u)\n", start,
	       start & r.mask);
	show(&r, "in(5)", spsc_in(&r, buf, 5), "added");
	show(&r, "in(5)", spsc_in(&r, buf, 5), "added");
	show(&r, "out(6)", spsc_out(&r, buf, 6), "removed");
	show(&r, "in(5)", spsc_in(&r, buf, 5), "added");
	spsc_free(&r);

	/* Part 2: indices wrapped to the size, as with circ_buf.h. */
	unsigned int head = 0, tail = 0, n = 0;

	while (CIRC_SPACE(head, tail, 8) > 0) {
		head = (head + 1) & 7;
		n++;
	}
	printf("masked indices (circ_buf.h style): full after %u of 8 slots\n",
	       n);

	/* Part 3: free-running indices with a size of 6. */
	unsigned int last = UINT_MAX;

	printf("6-slot ring using %%: index %u -> slot %u, next index %u -> "
	       "slot %u\n", last, last % 6, last + 1, (last + 1) % 6);
	return 0;
}
