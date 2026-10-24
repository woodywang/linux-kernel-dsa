// SPDX-License-Identifier: MIT
/*
 * hashtable.h - a teaching version of the kernel's fixed-size hash table
 * (include/linux/hashtable.h): an array of hlist heads whose size, a power
 * of two, is fixed when the table is declared.
 *
 * The table stores no keys and compares nothing.  hash_add() files a node
 * under a key's bucket; hash_for_each_possible() visits every object in
 * that bucket, and the caller must check the key itself.
 *
 * Left out on purpose: the RCU variants (hash_add_rcu() and friends),
 * which differ only in the list primitives they use.
 */
#ifndef HASHTABLE_H
#define HASHTABLE_H

#include "hash.h"
#include "hlist.h"

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

#define DEFINE_HASHTABLE(name, bits)					\
	struct hlist_head name[1 << (bits)] =				\
		{ [0 ... ((1 << (bits)) - 1)] = HLIST_HEAD_INIT }

#define HASH_SIZE(name)	(ARRAY_SIZE(name))
#define HASH_BITS(name)	(__builtin_ctz(HASH_SIZE(name)))

/* Use the 32-bit hash for keys that fit in 32 bits. */
#define hash_min(val, bits)						\
	(sizeof(val) <= 4 ? hash_32(val, bits) : hash_64(val, bits))

#define hash_add(table, node, key)					\
	hlist_add_head(node, &(table)[hash_min(key, HASH_BITS(table))])

static inline void hash_del(struct hlist_node *node)
{
	hlist_del_init(node);
}

/* Every object in @key's bucket, whatever its key. */
#define hash_for_each_possible(table, obj, member, key)			\
	hlist_for_each_entry(obj, &(table)[hash_min(key, HASH_BITS(table))], \
			     member)

/*
 * Every object in the table, bucket by bucket.  The inner loop leaves @obj
 * NULL when it runs off the end of a bucket, so testing @obj in the outer
 * loop makes a "break" in the body leave both loops.
 */
#define hash_for_each(table, bkt, obj, member)				\
	for ((bkt) = 0, obj = NULL;					\
	     obj == NULL && (bkt) < HASH_SIZE(table); (bkt)++)		\
		hlist_for_each_entry(obj, &(table)[bkt], member)

#endif
