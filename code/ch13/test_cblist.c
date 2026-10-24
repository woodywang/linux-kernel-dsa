// SPDX-License-Identifier: MIT
/*
 * test_cblist.c - randomized test of the segmented callback list.
 *
 *   cc -O2 -Wall -fsanitize=address,undefined -o test_cblist test_cblist.c
 *   ./test_cblist [steps] [seed]
 *
 * A simulated grace-period sequence number starts and ends grace periods
 * at random.  Callbacks are queued, numbered, advanced and extracted in
 * random order, and the list owner sometimes learns of a grace period's
 * end late.  The model records, for every callback, the grace period it
 * needs: the snapshot of the sequence number when it was queued.  After
 * every step the test checks
 *   - safety: no callback is numbered with an earlier grace period than
 *     it needs, and none reaches DONE before that grace period has ended;
 *   - order: callbacks come out in the order they went in;
 *   - shape: segment numbers increase, CB_WAIT is empty only if
 *     CB_NEXT_READY is, and the length is right;
 * and at the end that every callback came out exactly once.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "cblist.h"
#include "container_of.h"

struct cb {
	struct rcu_head rh;
	long id;
	unsigned long need;	/* grace period this callback must wait for */
	int out;		/* times extracted */
};

static struct cblist l;
static unsigned long gp = 0UL - 40;	/* wraps during the test */
static unsigned long seen;		/* last value passed to advance */
static long next_out;			/* FIFO check */

static unsigned long long rnd_state;
static unsigned int rnd(unsigned int n)
{
	rnd_state = rnd_state * 6364136223846793005ULL + 1442695040888963407ULL;
	return (unsigned int)(rnd_state >> 33) % n;
}

static void check(void)
{
	long n = 0;

	for (int i = CB_DONE; i < CB_NSEGS; i++) {
		struct rcu_head **pp = i == CB_DONE ? &l.head : l.tails[i - 1];

		for (; pp != l.tails[i]; pp = &(*pp)->next) {
			struct cb *c = container_of(*pp, struct cb, rh);

			assert(*pp);
			n++;
			if (i == CB_DONE)
				assert(rcu_seq_done_val(seen, c->need));
			else if (i != CB_NEXT)
				assert(!seq_lt(l.gp_seq[i], c->need));
		}
	}
	assert(*l.tails[CB_NEXT] == NULL);
	assert(n == l.len);
	if (cblist_segempty(&l, CB_WAIT))
		assert(cblist_segempty(&l, CB_NEXT_READY));
	else if (!cblist_segempty(&l, CB_NEXT_READY))
		assert(seq_lt(l.gp_seq[CB_WAIT], l.gp_seq[CB_NEXT_READY]));
}

static void extract(void)
{
	long n, cnt = 0;
	struct rcu_head *p = cblist_extract_done(&l, &n);

	for (; p; p = p->next, cnt++) {
		struct cb *c = container_of(p, struct cb, rh);

		assert(c->id == next_out++);
		assert(rcu_seq_done_val(seen, c->need));
		c->out++;
	}
	assert(cnt == n);
}

int main(int argc, char **argv)
{
	long steps = argc > 1 ? atol(argv[1]) : 200000;
	long nq = 0;
	struct cb *cbs;

	rnd_state = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	cbs = calloc(steps, sizeof(*cbs));
	if (!cbs)
		return 1;
	cblist_init(&l);
	seen = gp;
	for (long s = 0; s < steps; s++) {
		switch (rnd(8)) {
		case 0: case 1: case 2:			/* call_rcu() */
			cbs[nq].id = nq;
			/* The model's own rule: the end of the first grace
			 * period that starts after now. */
			cbs[nq].need = (gp & 1) ? gp + 3 : gp + 2;
			cblist_enqueue(&l, &cbs[nq].rh);
			if (rnd(2))
				cblist_accelerate(&l, rcu_seq_snap_val(gp));
			nq++;
			break;
		case 3:					/* start or end a GP */
			gp++;
			break;
		case 4:					/* number callbacks */
			cblist_accelerate(&l, rcu_seq_snap_val(gp));
			break;
		case 5: case 6:				/* notice GP ends */
			seen = gp;
			cblist_advance(&l, seen);
			break;
		case 7:
			extract();
			break;
		}
		check();
	}
	/* Drain: keep running grace periods until everything is out. */
	for (int round = 0; l.len && round < 10; round++) {
		cblist_accelerate(&l, rcu_seq_snap_val(gp));
		gp += 2;
		seen = gp;
		cblist_advance(&l, seen);
		extract();
		check();
	}
	assert(l.len == 0 && next_out == nq);
	for (long i = 0; i < nq; i++)
		assert(cbs[i].out == 1);
	printf("test_cblist: %ld steps, %ld callbacks: ok\n", steps, nq);
	free(cbs);
	return 0;
}
