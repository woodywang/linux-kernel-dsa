// SPDX-License-Identifier: MIT
/* list_sort.h - a teaching version of the kernel's linked-list merge sort. */
#ifndef LIST_SORT_H
#define LIST_SORT_H

#include "list.h"

/*
 * @cmp returns > 0 if a must sort after b, and <= 0 otherwise.  It is
 * always called with the element that came first in the input as a, so a
 * comparison that returns only 0 or 1 is enough for a stable sort.
 */
typedef int (*list_cmp_func_t)(void *priv, const struct list_head *a,
			       const struct list_head *b);

void list_sort(void *priv, struct list_head *head, list_cmp_func_t cmp);

#endif /* LIST_SORT_H */
