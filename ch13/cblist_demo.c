// SPDX-License-Identifier: MIT
/*
 * cblist_demo.c - one CPU's callback list through three grace periods.
 *
 *   cc -O2 -Wall -o cblist_demo cblist_demo.c && ./cblist_demo
 *
 * The grace-period sequence number counts in steps of two: even means
 * idle, odd means a grace period is running, and a grace period that
 * starts at 2k + 1 is complete when the number reaches 2k + 2.  The list
 * owner learns of grace-period ends late, as a CPU in the kernel does:
 * only when it calls cblist_advance().
 */
#include <stdio.h>
#include "cblist.h"
#include "container_of.h"

struct cb {
	struct rcu_head rh;
	char name;
};

static struct cblist l;
static unsigned long gp = 4;

/* Print segment i: its callbacks, and the grace period they wait for. */
static int show_seg(char *out, int i)
{
	struct rcu_head **pp = i == CB_DONE ? &l.head : l.tails[i - 1];
	char buf[32];
	int n = 0;

	for (; pp != l.tails[i]; pp = &(*pp)->next)
		n += snprintf(buf + n, sizeof(buf) - n, "%c ",
			      container_of(*pp, struct cb, rh)->name);
	if (!n)
		n = snprintf(buf, sizeof(buf), "-");
	else if (i == CB_WAIT || i == CB_NEXT_READY)
		n += snprintf(buf + n, sizeof(buf) - n, "@%lu", l.gp_seq[i]);
	return sprintf(out, "%-12s", buf);
}

static void show(const char *step)
{
	char line[128];
	int n = sprintf(line, "%-24s %-4lu", step, gp);

	for (int i = 0; i < CB_NSEGS; i++)
		n += show_seg(line + n, i);
	while (n > 0 && line[n - 1] == ' ')
		line[--n] = '\0';	/* no trailing blanks */
	puts(line);
}

static void queue(struct cb *c)
{
	cblist_enqueue(&l, &c->rh);
	cblist_accelerate(&l, rcu_seq_snap_val(gp));
}

static void invoke(void)
{
	long n;

	cblist_extract_done(&l, &n);
}

int main(void)
{
	struct cb a = { .name = 'a' }, b = { .name = 'b' }, c = { .name = 'c' };
	struct cb d = { .name = 'd' }, e = { .name = 'e' };

	cblist_init(&l);
	printf("%-24s %-4s%-12s%-12s%-12s%s\n", "step", "gp", "DONE",
	       "WAIT", "NEXT_READY", "NEXT");
	queue(&a);
	queue(&b);
	show("queue a, b");
	gp++;
	show("6 starts");
	queue(&c);
	show("queue c");
	gp++;
	show("6 ends, unnoticed");
	gp++;
	queue(&d);
	show("8 starts; queue d");
	cblist_advance(&l, gp);
	cblist_accelerate(&l, rcu_seq_snap_val(gp));
	show("advance, accelerate");
	invoke();
	queue(&e);
	show("invoke a, b; queue e");
	gp++;
	cblist_advance(&l, gp);
	show("8 ends; advance");
	invoke();
	gp += 2;
	cblist_advance(&l, gp);
	show("invoke c; 10 runs");
	invoke();
	show("invoke d, e");
	return 0;
}
