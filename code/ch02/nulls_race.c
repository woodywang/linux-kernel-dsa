// SPDX-License-Identifier: MIT
/*
 * nulls_race.c - why lock-free hash lookups use hlist_nulls.
 *
 * A reader walks a hash chain without a lock.  Objects can be freed and
 * immediately reused for a different key (the kernel's SLAB_TYPESAFE_BY_RCU
 * caches allow this), so the object the reader stands on may move to
 * another chain mid-walk.  The reader then finishes its walk in the wrong
 * chain.  With a plain hlist it ends at NULL and wrongly reports "not
 * found"; with hlist_nulls the end marker names the chain, so the reader
 * notices and restarts.
 *
 * The race is replayed step by step in one thread, so the output is
 * deterministic.  What it leaves out on purpose: real concurrency, RCU,
 * and the reference counting a real lookup does before trusting a match.
 *
 *   cc -O2 -Wall -o nulls_race nulls_race.c && ./nulls_race
 */
#include <stdio.h>
#include "list.h"
#include "list_nulls.h"

#define NBUCKETS 8

/* ---- plain hlist ---------------------------------------------------- */

struct pobj {
	unsigned int key;
	struct hlist_node node;
};

static struct hlist_head ptable[NBUCKETS];

static void plain_race(void)
{
	static struct pobj o[4] = { { .key = 27 }, { .key = 19 },
				    { .key = 11 }, { .key = 21 } };
	unsigned int want = 27, slot = want % NBUCKETS;
	struct hlist_node *pos;

	for (int i = 0; i < 4; i++)	/* chain 3: 11 19 27; chain 5: 21 */
		hlist_add_head(&o[i].node, &ptable[o[i].key % NBUCKETS]);

	pos = ptable[slot].first;	/* reader: stands on 11 ... */

	hlist_del(&o[2].node);		/* writer: free 11, reuse it as 13 */
	o[2].key = 13;
	hlist_add_head(&o[2].node, &ptable[13 % NBUCKETS]);

	for (; pos; pos = pos->next) {	/* ... reader resumes */
		struct pobj *p = hlist_entry(pos, struct pobj, node);

		if (p->key == want)
			break;
	}
	printf("hlist:       lookup %u in chain %u: %s\n", want, slot,
	       pos ? "found" : "not found (wrong: 27 is in chain 3)");
}

/* ---- hlist_nulls ---------------------------------------------------- */

struct nobj {
	unsigned int key;
	struct hlist_nulls_node node;
};

static struct hlist_nulls_head ntable[NBUCKETS];

/* One lookup; *resume, if set, is where a paused reader continues. */
static struct nobj *lookup(unsigned int key, struct hlist_nulls_node *resume)
{
	unsigned int slot = key % NBUCKETS;
	struct hlist_nulls_node *pos;
	struct nobj *obj;

begin:
	pos = resume ? resume : ntable[slot].first;
	resume = NULL;
	for (; !is_a_nulls(pos); pos = pos->next) {
		obj = hlist_nulls_entry(pos, struct nobj, node);
		if (obj->key == key)
			return obj;
	}
	if (get_nulls_value(pos) != slot) {
		printf("             walk ended at nulls %lu, expected %u: "
		       "restart\n", get_nulls_value(pos), slot);
		goto begin;
	}
	return NULL;
}

static void nulls_race(void)
{
	static struct nobj o[4] = { { .key = 27 }, { .key = 19 },
				    { .key = 11 }, { .key = 21 } };
	unsigned int want = 27, slot = want % NBUCKETS;
	struct hlist_nulls_node *pos;

	for (unsigned int i = 0; i < NBUCKETS; i++)
		INIT_HLIST_NULLS_HEAD(&ntable[i], i);
	for (int i = 0; i < 4; i++)
		hlist_nulls_add_head(&o[i].node, &ntable[o[i].key % NBUCKETS]);

	pos = ntable[slot].first;	/* reader: stands on 11 ... */

	hlist_nulls_del(&o[2].node);	/* writer: free 11, reuse it as 13 */
	o[2].key = 13;
	hlist_nulls_add_head(&o[2].node, &ntable[13 % NBUCKETS]);

	printf("hlist_nulls: lookup %u in chain %u:\n", want, slot);
	printf("             %s\n", lookup(want, pos) ? "found" : "not found");
}

int main(void)
{
	plain_race();
	nulls_race();
	return 0;
}
