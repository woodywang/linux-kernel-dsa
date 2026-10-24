// SPDX-License-Identifier: MIT
/*
 * reclaim.h - interface of the page-reclaim simulator (reclaim.c).
 *
 * A trace is an array of accesses.  Each access names a page (an integer
 * below MAX_PAGES) and says how the program touched it: through a page
 * table (a "mapped" access, which the kernel does not see as it happens;
 * the hardware only sets the accessed bit) or through a file descriptor
 * (an "fd" access such as read(), which the kernel sees and records).
 */
#ifndef RECLAIM_H
#define RECLAIM_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_PAGES	(1u << 16)
#define FD_ACCESS	(1u << 31)	/* flag in a trace entry */

static inline uint32_t acc_page(uint32_t a) { return a & ~FD_ACCESS; }
static inline bool acc_fd(uint32_t a) { return a & FD_ACCESS; }

enum policy {
	POL_LRU,	/* exact LRU: move to the front on every hit */
	POL_CLOCK,	/* one list, a referenced bit, second chance */
	POL_2LIST,	/* inactive + active lists, as in mm/vmscan.c */
	POL_2LIST_WS,	/* the same, plus shadow entries (mm/workingset.c) */
	POL_MGLRU,	/* a simplified multi-generational LRU */
	POL_OPT,	/* Belady: evict the page used furthest in the future */
	NR_POLICIES
};

extern const char *const policy_name[NR_POLICIES];

struct sim_stats {
	long hits, misses, evictions;
	long activations;		/* pages moved to the active list */
	long refault_activations;	/* refaults activated by a shadow */
	long agings;			/* MGLRU: new generations created */
};

struct sim;

struct sim *sim_new(enum policy pol, int capacity);
void sim_free(struct sim *s);
/* Access trace[i]; returns true on a hit.  OPT needs sim_set_trace(). */
bool sim_access(struct sim *s, const uint32_t *trace, long n, long i);
void sim_set_trace(struct sim *s, const uint32_t *trace, long n);
const struct sim_stats *sim_stats(const struct sim *s);
/* MGLRU: create a new generation now, as kswapd might (Exercise 17.3). */
void sim_age(struct sim *s);
/* Check the policy's invariants; aborts on failure. */
void sim_check(const struct sim *s);

/* Run a whole trace; returns the number of hits. */
long sim_run(enum policy pol, int capacity, const uint32_t *trace, long n,
	     struct sim_stats *st);

/* Shadow entries, exposed for the tests. */
unsigned long pack_shadow(int memcgid, int nid, unsigned long eviction,
			  bool workingset);
void unpack_shadow(unsigned long shadow, int *memcgid, int *nid,
		   unsigned long *eviction, bool *workingset);
unsigned long refault_distance(unsigned long now, unsigned long eviction);

#endif
