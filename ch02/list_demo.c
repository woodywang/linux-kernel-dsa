// SPDX-License-Identifier: MIT
/*
 * list_demo.c - what the list corruption checks catch, and what they don't.
 * Built with -DLIST_HARDENED, so list.h checks every add and delete.
 *
 *   cc -O2 -Wall -DLIST_HARDENED -o list_demo list_demo.c && ./list_demo
 */
#include <stdio.h>
#include "list.h"

struct item {
	char name;
	struct list_head node;
};

static void show(const char *label, struct list_head *head)
{
	struct item *it;

	printf("  %s:", label);
	list_for_each_entry(it, head, node)
		printf(" %c", it->name);
	printf("\n");
}

int main(void)
{
	LIST_HEAD(a);
	LIST_HEAD(b);
	struct item x = { .name = 'x' }, y = { .name = 'y' }, z = { .name = 'z' };

	printf("1. delete twice with list_del()\n");
	list_add_tail(&x.node, &a);
	list_del(&x.node);
	printf("  x.next is LIST_POISON1? %s\n",
	       x.node.next == LIST_POISON1 ? "yes" : "no");
	list_del(&x.node);			/* second delete: refused */

	printf("2. delete twice with list_del_init()\n");
	list_add_tail(&x.node, &a);
	list_del_init(&x.node);
	list_del_init(&x.node);			/* a no-op on an empty node */
	printf("  x is an empty list? %s\n", list_empty(&x.node) ? "yes" : "no");

	printf("3. add the same node twice in a row\n");
	list_add(&x.node, &a);
	list_add(&x.node, &a);			/* refused */
	show("a", &a);

	printf("4. add a node to a second list without removing it\n");
	list_add_tail(&y.node, &a);
	list_add_tail(&z.node, &a);
	show("a", &a);
	list_add_tail(&y.node, &b);		/* not caught: b's links look fine */
	show("b", &b);
	printf("  now delete z from a\n");
	list_del(&z.node);			/* caught here, one step later */

	printf("5. overwritten links: the unlink write primitive\n");
	struct list_head w1 = { NULL, NULL }, w2 = { NULL, NULL };
	struct item v = { .name = 'v' };

	v.node.next = &w1;			/* as if an attacker */
	v.node.prev = &w2;			/* overwrote both links */
	list_del(&v.node);			/* refused (then poisoned) */
	printf("  checked list_del: w1.prev %s, w2.next %s\n",
	       w1.prev ? "overwritten" : "untouched",
	       w2.next ? "overwritten" : "untouched");
	__list_del(&w2, &w1);			/* the raw primitive */
	printf("  raw __list_del:   w1.prev %s, w2.next %s\n",
	       w1.prev ? "overwritten" : "untouched",
	       w2.next ? "overwritten" : "untouched");

	printf("corruptions caught: %ld\n", *list_corruptions());
	return 0;
}
