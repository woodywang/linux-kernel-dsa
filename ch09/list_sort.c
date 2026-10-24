// SPDX-License-Identifier: MIT
/*
 * list_sort.c - a teaching version of the kernel's list_sort()
 * (lib/list_sort.c): a stable, bottom-up merge sort of an intrusive list
 * that needs no extra memory and no recursion.
 *
 * The idea, as in the kernel:
 *   - Break the circular list into a NULL-terminated singly linked list.
 *     The prev pointers are then free, and the first element of each
 *     sorted sublist uses its prev pointer to chain the "pending" sublists
 *     together, newest (and smallest) first.
 *   - Move input elements to pending one at a time.  A counter of pending
 *     elements decides when to merge: incrementing count from x to x + 1
 *     clears the trailing 1 bits of x and sets the next bit k; unless count
 *     is becoming exactly 2^k, that is the moment to merge the two newest
 *     sublists of size 2^k.  No merge is ever worse than 2:1.
 *   - At the end of the input, merge the pending sublists, smallest first.
 *
 * What it leaves out on purpose: the kernel merges the last two sublists
 * and rebuilds the prev links in one pass (merge_final()); here the final
 * merge is an ordinary merge followed by a separate pass that rebuilds the
 * prev links.  It also omits the 6.18 kernel's periodic cmp(priv, b, b)
 * calls, which exist only so that a caller's cmp() can reschedule.
 */
#include "list_sort.h"

/* Merge two NULL-terminated sorted lists; on ties, @a's element first. */
static struct list_head *merge(void *priv, list_cmp_func_t cmp,
			       struct list_head *a, struct list_head *b)
{
	struct list_head *head = NULL, **tail = &head;

	while (a && b) {
		if (cmp(priv, a, b) <= 0) {	/* stability: prefer a */
			*tail = a;
			a = a->next;
		} else {
			*tail = b;
			b = b->next;
		}
		tail = &(*tail)->next;
	}
	*tail = a ? a : b;
	return head;
}

void list_sort(void *priv, struct list_head *head, list_cmp_func_t cmp)
{
	struct list_head *in = head->next, *pending = NULL, *list, *prev;
	size_t count = 0;		/* elements in pending sublists */

	if (in == head->prev)		/* zero or one element */
		return;
	head->prev->next = NULL;	/* now a NULL-terminated list */

	do {
		struct list_head **slot = &pending;
		size_t bits;

		/* Skip one pending sublist per trailing 1 bit of count. */
		for (bits = count; bits & 1; bits >>= 1)
			slot = &(*slot)->prev;
		/* Any higher bit set: merge the two sublists at *slot. */
		if (bits) {
			struct list_head *newer = *slot, *older = newer->prev;
			struct list_head *merged;

			merged = merge(priv, cmp, older, newer);
			merged->prev = older->prev;
			*slot = merged;
		}
		/* Move one input element to pending as a sublist of size 1. */
		list = in;
		in = in->next;
		list->next = NULL;
		list->prev = pending;
		pending = list;
		count++;
	} while (in);

	/* End of input: merge everything, smallest sublists first. */
	list = pending;
	for (pending = pending->prev; pending; pending = pending->prev)
		list = merge(priv, cmp, pending, list);

	/* Rebuild the prev links and close the circle through head. */
	prev = head;
	for (head->next = list; list; list = list->next) {
		list->prev = prev;
		prev = list;
	}
	prev->next = head;
	head->prev = prev;
}
