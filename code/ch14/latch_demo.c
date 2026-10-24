// SPDX-License-Identifier: MIT
/*
 * latch_demo.c - a reader that interrupts its own writer.
 *
 *   cc -O2 -Wall -o latch_demo latch_demo.c && ./latch_demo
 *
 * The kernel uses a latch where a reader can run in the middle of a
 * write on the same CPU, typically from an NMI.  This demo plays that
 * scenario on one thread: the writer calls an "interrupt" function in the
 * middle of its work, and the interrupt tries to read.
 */
#include <inttypes.h>
#include <stdio.h>
#include "latch.h"

static struct ltable lt;

static void interrupt(void)
{
	unsigned seq = read_seqcount_latch(&lt.seq);
	uint64_t v = 0;
	bool found = lt_find(&lt, 30, &v);

	printf("  interrupt: sequence %u, reads copy %u: key 30 %s\n",
	       seq, seq & 1, found ? "present" : "absent");
}

int main(void)
{
	struct seqcount sc;

	/* A plain seqcount, interrupted in the middle of a write. */
	seqcount_init(&sc);
	write_seqcount_begin(&sc);
	printf("seqcount: interrupt during a write sees sequence %u (odd):\n"
	       "  a reader would wait for a writer that cannot run until "
	       "the reader returns\n", raw_read_seqcount(&sc));
	write_seqcount_end(&sc);

	/* A latched table, interrupted while each copy is rewritten. */
	for (uint64_t k = 10; k <= 80; k += 10)
		if (k != 30)
			lt_insert(&lt, k, k * 10);
	lt_interrupt = interrupt;
	printf("latch: inserting key 30\n");
	lt_insert(&lt, 30, 300);
	printf("latch: erasing key 30\n");
	lt_erase(&lt, 30);
	return 0;
}
