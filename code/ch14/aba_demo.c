// SPDX-License-Identifier: MIT
/*
 * aba_demo.c - why llist_del_first() allows only one consumer.
 *
 *   cc -O2 -Wall -o aba_demo aba_demo.c && ./aba_demo
 *
 * Replays, step by step and on one thread, the interleaving that the
 * kernel's llist.h warns about: consumer 1 is preempted in the middle of
 * llist_del_first(), after reading first and first->next but before its
 * compare-and-swap; meanwhile another consumer pops the same node, and it
 * is pushed again.  The head pointer is back to the value consumer 1 saw,
 * so its compare-and-swap succeeds, and installs a stale next pointer.
 */
#include <stdio.h>
#include "container_of.h"
#include "llist.h"

struct item {
	struct llist_node node;
	char name;
};

static struct item A = { .name = 'A' }, B = { .name = 'B' },
		   C = { .name = 'C' }, D = { .name = 'D' };

static char name(struct llist_node *n)
{
	return n ? container_of(n, struct item, node)->name : '-';
}

static void show(const char *who, const char *what, struct llist_head *h)
{
	struct llist_node *n;
	int len = 0;

	len += printf("%-11s %s", who, what);
	printf("%*s list:", 44 - len, "");
	llist_for_each(n, atomic_load(&h->first))
		printf(" %c", name(n));
	printf("\n");
}

int main(void)
{
	struct llist_head head;
	struct llist_node *entry, *next;
	char buf[64];

	init_llist_head(&head);
	llist_add(&C.node, &head);
	llist_add(&B.node, &head);
	llist_add(&A.node, &head);
	show("", "start", &head);

	/* Consumer 1: the first half of llist_del_first(). */
	entry = atomic_load(&head.first);
	next = entry->next;
	snprintf(buf, sizeof(buf), "reads first=%c next=%c", name(entry),
		 name(next));
	show("consumer 1:", buf, &head);

	/* Consumer 2 and a producer run while consumer 1 is preempted. */
	snprintf(buf, sizeof(buf), "del_first -> %c",
		 name(llist_del_first(&head)));
	show("consumer 2:", buf, &head);
	llist_add(&D.node, &head);
	show("producer:", "add D", &head);
	llist_add(&A.node, &head);
	show("producer:", "add A again", &head);

	/* Consumer 1 resumes: the second half of llist_del_first(). */
	bool ok = atomic_compare_exchange_strong(&head.first, &entry, next);
	snprintf(buf, sizeof(buf), "cmpxchg(first, %c, %c) %s", name(entry),
		 name(next), ok ? "succeeds" : "fails");
	show("consumer 1:", buf, &head);
	printf("D is %s\n", head.first == &B.node ? "lost" : "still there");
	return 0;
}
