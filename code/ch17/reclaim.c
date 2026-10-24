// SPDX-License-Identifier: MIT
/*
 * reclaim.c - a simulator of the page-replacement policies in Chapter 17.
 *
 * Memory holds `capacity` pages.  Each policy decides which page to evict
 * when a missing page must be brought in.  Pages are described by an array
 * of struct page indexed by page number, like the kernel's memory map, and
 * the policies' lists are intrusive lists through page->lru (Chapter 2).
 *
 * What it models:
 *   - exact LRU, single-list CLOCK, and Belady's offline optimum (OPT);
 *   - the kernel's two-list LRU: accesses through page tables only set an
 *     accessed bit, accesses through file descriptors go through
 *     mark_accessed(); reclaim scans the inactive list's tail and
 *     deactivates from the active list when the inactive list is low;
 *   - workingset detection: a shadow entry with an eviction timestamp is
 *     left behind for each evicted page, and a refault whose distance is
 *     no larger than the active list is activated at once;
 *   - a simplified multi-generational LRU: up to four generations, aging
 *     that promotes pages by rewriting their generation number only,
 *     eviction that sorts lazily, tiers for fd accesses, and the feedback
 *     loop that decides which tiers to protect.
 *
 * What it leaves out on purpose: anonymous pages and swap (every page is
 * a clean file page), dirty pages and writeback, NUMA nodes, memory
 * cgroups, zones, folios larger than one page, batching, locking, the
 * page-table walk's Bloom filters (see bloom.h), and kswapd's watermarks:
 * reclaim frees exactly one page when memory is full.  The results show
 * how the policies behave on synthetic traces, not how the kernel
 * performs.
 */
#include "reclaim.h"
#include "ilist.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

const char *const policy_name[NR_POLICIES] = {
	"LRU", "CLOCK", "2-list", "2-list+ws", "MGLRU", "OPT",
};

/* ---- list helpers that ilist.h leaves out -------------------------- */

static inline void list_move(struct list_head *n, struct list_head *head)
{
	list_del(n);
	list_add(n, head);
}

static inline void list_move_tail(struct list_head *n, struct list_head *head)
{
	list_del(n);
	list_add_tail(n, head);
}

#define lru_to_page(head) list_entry((head)->prev, struct page, lru)

/* ---- the page ------------------------------------------------------ */

struct page {
	struct list_head lru;	/* on exactly one list while resident */
	bool resident;
	bool accessed;		/* the PTE's accessed bit, set by "hardware" */
	bool referenced;	/* PG_referenced */
	bool active;		/* PG_active (two-list policies) */
	bool workingset;	/* PG_workingset */
	unsigned char gen;	/* MGLRU: generation + 1 while on a list */
	unsigned char refs;	/* MGLRU: extra fd accesses, 0..REFS_MAX */
	unsigned long shadow;	/* value entry left at eviction, or 0 */
	long next_use;		/* OPT: position of the next access */
};

#define MIN_NR_GENS	2
#define MAX_NR_GENS	4
#define MAX_NR_TIERS	4
#define REFS_MAX	3	/* two bits of counter, as LRU_REFS_WIDTH */
#define MIN_LRU_BATCH	64

struct heap_ent {
	long key;		/* next use */
	unsigned int id;
};

struct sim {
	enum policy pol;
	int cap, nr_resident;
	struct page *pg;
	struct sim_stats st;

	/* LRU and CLOCK: lists[0]; two-list: inactive and active */
	struct list_head lists[2];
	struct list_head *inactive, *active;
	long nr_inactive, nr_active;
	unsigned long nonresident_age;	/* evictions + activations */
	bool refaulted;			/* a refault since the last reclaim */

	/* MGLRU */
	struct list_head gens[MAX_NR_GENS];
	long nr_gen[MAX_NR_GENS];
	unsigned long max_seq, min_seq;
	long evicted[MAX_NR_TIERS], refaulted_t[MAX_NR_TIERS];
	long protected[MAX_NR_TIERS];
	long avg_refaulted[MAX_NR_TIERS], avg_total[MAX_NR_TIERS];

	/* OPT */
	const uint32_t *trace;
	long *next;
	struct heap_ent *heap;
	long heap_n;
};

/* ---- shadow entries ------------------------------------------------ */

/*
 * The layout follows mm/workingset.c on a 64-bit machine with up to 64
 * NUMA nodes: [ eviction | memcg id | node | workingset ] and then one
 * more bit, set, that marks the word as an XArray value entry rather than
 * a pointer (Chapter 7).
 */
#define WORKINGSET_SHIFT	1
#define NODES_SHIFT		6
#define MEMCG_ID_SHIFT		16
#define EVICTION_SHIFT		(1 + WORKINGSET_SHIFT + NODES_SHIFT + MEMCG_ID_SHIFT)
#define EVICTION_MASK		(~0UL >> EVICTION_SHIFT)

unsigned long pack_shadow(int memcgid, int nid, unsigned long eviction,
			  bool workingset)
{
	eviction &= EVICTION_MASK;
	eviction = (eviction << MEMCG_ID_SHIFT) | memcgid;
	eviction = (eviction << NODES_SHIFT) | nid;
	eviction = (eviction << WORKINGSET_SHIFT) | workingset;

	return (eviction << 1) | 1;		/* xa_mk_value() */
}

void unpack_shadow(unsigned long shadow, int *memcgid, int *nid,
		   unsigned long *eviction, bool *workingset)
{
	unsigned long entry = shadow >> 1;	/* xa_to_value() */

	*workingset = entry & ((1UL << WORKINGSET_SHIFT) - 1);
	entry >>= WORKINGSET_SHIFT;
	*nid = entry & ((1UL << NODES_SHIFT) - 1);
	entry >>= NODES_SHIFT;
	*memcgid = entry & ((1UL << MEMCG_ID_SHIFT) - 1);
	entry >>= MEMCG_ID_SHIFT;
	*eviction = entry;
}

/* Unsigned subtraction, masked: correct across a wraparound of the clock. */
unsigned long refault_distance(unsigned long now, unsigned long eviction)
{
	return (now - eviction) & EVICTION_MASK;
}

/* ---- exact LRU and CLOCK ------------------------------------------- */

static void lru_hit(struct sim *s, struct page *p)
{
	if (s->pol == POL_LRU)
		list_move(&p->lru, &s->lists[0]);	/* work on every hit */
	else
		p->referenced = true;			/* CLOCK: just a bit */
}

static struct page *lru_victim(struct sim *s)
{
	for (;;) {
		struct page *p = lru_to_page(&s->lists[0]);

		if (s->pol == POL_CLOCK && p->referenced) {
			p->referenced = false;		/* second chance */
			list_move(&p->lru, &s->lists[0]);
			continue;
		}
		return p;
	}
}

/* ---- the two-list LRU ---------------------------------------------- */

static void age_nonresident(struct sim *s)
{
	s->nonresident_age++;
}

static void activate(struct sim *s, struct page *p)
{
	list_move(&p->lru, s->active);
	p->active = true;
	s->nr_inactive--;
	s->nr_active++;
	s->st.activations++;
	age_nonresident(s);
}

/*
 * An access through a file descriptor:
 *   inactive,unreferenced -> inactive,referenced
 *   inactive,referenced   -> active,unreferenced
 *   active,unreferenced   -> active,referenced
 */
static void mark_accessed(struct sim *s, struct page *p)
{
	if (!p->referenced) {
		p->referenced = true;
	} else if (!p->active) {
		activate(s, p);
		p->referenced = false;
	}
}

static void two_list_hit(struct sim *s, struct page *p, bool fd)
{
	if (fd)
		mark_accessed(s, p);
	else
		p->accessed = true;	/* the hardware sets the PTE bit */
}

/* Is a refault recent enough to compete with the active list? */
static bool workingset_test_recent(struct sim *s, unsigned long shadow,
				   bool *workingset)
{
	unsigned long eviction;
	int memcgid, nid;

	unpack_shadow(shadow, &memcgid, &nid, &eviction, workingset);
	return refault_distance(s->nonresident_age, eviction) <=
	       (unsigned long)s->nr_active;
}

enum page_references { PAGEREF_RECLAIM, PAGEREF_KEEP, PAGEREF_ACTIVATE };

/* What to do with the page at the tail of the inactive list. */
static enum page_references check_references(struct page *p)
{
	bool referenced_ptes = p->accessed;
	bool referenced_page = p->referenced;

	p->accessed = false;		/* the rmap walk clears the bits */
	p->referenced = false;
	if (referenced_ptes) {
		/* Mapped pages start with a reference from the fault, so
		 * look twice: mark it now, activate it next time. */
		p->referenced = true;
		if (referenced_page)
			return PAGEREF_ACTIVATE;
		return PAGEREF_KEEP;
	}
	/* Unmapped: an fd access would already have activated it. */
	return PAGEREF_RECLAIM;
}

/* Move the active list's oldest page to the inactive list. */
static void shrink_active_one(struct sim *s)
{
	struct page *p = lru_to_page(s->active);

	p->accessed = false;	/* referenced or not, it is demoted */
	p->active = false;
	p->workingset = true;	/* it was active once */
	list_move(&p->lru, s->inactive);
	s->nr_active--;
	s->nr_inactive++;
}

static bool inactive_is_low(const struct sim *s)
{
	return s->nr_inactive < s->nr_active;	/* target ratio 1:1 */
}

static struct page *two_list_victim(struct sim *s)
{
	for (;;) {
		if (s->nr_active && (inactive_is_low(s) || s->refaulted)) {
			shrink_active_one(s);
			s->refaulted = false;
		}

		struct page *p = lru_to_page(s->inactive);

		switch (check_references(p)) {
		case PAGEREF_ACTIVATE:
			activate(s, p);
			break;
		case PAGEREF_KEEP:
			list_move(&p->lru, s->inactive);
			break;
		case PAGEREF_RECLAIM:
			if (s->pol == POL_2LIST_WS)
				p->shadow = pack_shadow(0, 0, s->nonresident_age,
							p->workingset);
			age_nonresident(s);
			s->nr_inactive--;
			return p;
		}
	}
}

static void two_list_add(struct sim *s, struct page *p, bool fd)
{
	bool workingset;

	p->accessed = !fd;	/* a fault leaves the PTE accessed */
	if (p->shadow && workingset_test_recent(s, p->shadow, &workingset)) {
		p->active = true;	/* activate the refault at once */
		p->workingset = workingset;
		s->st.refault_activations++;
		s->refaulted = true;
		age_nonresident(s);
	}
	p->shadow = 0;
	if (p->active) {
		list_add(&p->lru, s->active);
		s->nr_active++;
	} else {
		list_add(&p->lru, s->inactive);
		s->nr_inactive++;
	}
	if (fd)
		mark_accessed(s, p);
}

/* ---- the multi-generational LRU ------------------------------------ */

static int gen_from_seq(unsigned long seq)
{
	return seq % MAX_NR_GENS;
}

static int nr_gens(const struct sim *s)
{
	return s->max_seq - s->min_seq + 1;
}

/* Accesses through fds: PG_referenced first, then a counter, then
 * PG_workingset.  folio_lru_refs() counts all of them. */
static int page_refs(const struct page *p)
{
	return p->referenced ? p->refs + 1 : 0;
}

static int order_base_2(int n)
{
	int k = 0;

	while ((1 << k) < n)
		k++;
	return k;
}

static int tier_of(const struct page *p)
{
	return p->workingset ? MAX_NR_TIERS - 1 : order_base_2(page_refs(p));
}

static void lru_gen_inc_refs(struct page *p)
{
	if (!p->referenced)
		p->referenced = true;
	else if (p->refs < REFS_MAX)
		p->refs++;
	else
		p->workingset = true;
}

static void set_gen(struct sim *s, struct page *p, int gen)
{
	if (p->gen)
		s->nr_gen[p->gen - 1]--;
	p->gen = gen + 1;
	s->nr_gen[gen]++;
}

/* The I term: halve the running averages and fold in this generation. */
static void reset_ctrl_pos(struct sim *s)
{
	for (int t = 0; t < MAX_NR_TIERS; t++) {
		s->avg_refaulted[t] = (s->avg_refaulted[t] + s->refaulted_t[t]) / 2;
		s->avg_total[t] = (s->avg_total[t] + s->protected[t] +
				   s->evicted[t]) / 2;
		s->refaulted_t[t] = s->evicted[t] = s->protected[t] = 0;
	}
}

/* Protect a page by moving it one generation up from the oldest. */
static int page_inc_gen(struct sim *s, struct page *p)
{
	int old_gen = gen_from_seq(s->min_seq);
	int new_gen = p->gen - 1;

	if (new_gen != old_gen)		/* already promoted by the aging */
		return new_gen;
	new_gen = (old_gen + 1) % MAX_NR_GENS;
	set_gen(s, p, new_gen);
	p->referenced = false;
	p->refs = 0;
	return new_gen;
}

static void inc_min_seq(struct sim *s)
{
	struct list_head *head = &s->gens[gen_from_seq(s->min_seq)];

	while (!list_empty(head)) {
		struct page *p = lru_to_page(head);
		int gen = page_inc_gen(s, p);

		list_move_tail(&p->lru, &s->gens[gen]);
	}
	reset_ctrl_pos(s);
	s->min_seq++;
}

/* Drop generations that eviction has emptied. */
static void try_to_inc_min_seq(struct sim *s)
{
	while (nr_gens(s) > MIN_NR_GENS &&
	       list_empty(&s->gens[gen_from_seq(s->min_seq)])) {
		reset_ctrl_pos(s);
		s->min_seq++;
	}
}

/* Seen young by the aging: second chance, then promotion. */
static void page_update_gen(struct sim *s, struct page *p, int gen)
{
	if (!p->referenced && !p->workingset) {
		p->referenced = true;
		return;
	}
	set_gen(s, p, gen);		/* the list is fixed up later */
	p->workingset = true;
	p->referenced = false;
	p->refs = 0;
}

static void lru_gen_age(struct sim *s)
{
	int young = gen_from_seq(s->max_seq);

	if (nr_gens(s) == MAX_NR_GENS)
		inc_min_seq(s);		/* make room in the window */

	/* The "page-table walk": test and clear every accessed bit. */
	for (int g = 0; g < MAX_NR_GENS; g++) {
		struct page *p;

		list_for_each_entry(p, &s->gens[g], lru) {
			if (p->accessed) {
				p->accessed = false;
				page_update_gen(s, p, young);
			}
		}
	}
	s->max_seq++;
	s->st.agings++;
}

void sim_age(struct sim *s)
{
	if (s->pol == POL_MGLRU)
		lru_gen_age(s);
}

/* Does a tier refault less often than tier 0 (allowing a 2:3 margin)? */
static bool positive_ctrl_err(long sp_ref, long sp_tot, long pv_ref, long pv_tot)
{
	return pv_ref < MIN_LRU_BATCH ||
	       pv_ref * (sp_tot + MIN_LRU_BATCH) * 2 <= (sp_ref + 1) * pv_tot * 3;
}

/* The highest tier that may be evicted; tiers above it are protected. */
static int get_tier_idx(const struct sim *s)
{
	long sp_ref = s->avg_refaulted[0] + s->refaulted_t[0];
	long sp_tot = s->avg_total[0] + s->protected[0] + s->evicted[0];
	int tier;

	for (tier = 1; tier < MAX_NR_TIERS; tier++) {
		long pv_ref = s->avg_refaulted[tier] + s->refaulted_t[tier];
		long pv_tot = s->avg_total[tier] + s->protected[tier] +
			      s->evicted[tier];

		if (!positive_ctrl_err(sp_ref, sp_tot, pv_ref, pv_tot))
			break;
	}
	return tier - 1;
}

/* Lazy sorting: returns true if the page stays in memory. */
static bool sort_page(struct sim *s, struct page *p, int tier_idx)
{
	int gen = p->gen - 1, tier = tier_of(p);
	bool saturated = page_refs(p) + p->workingset == REFS_MAX + 2;

	if (gen != gen_from_seq(s->min_seq)) {		/* promoted */
		list_move(&p->lru, &s->gens[gen]);
		return true;
	}
	if (tier > tier_idx || saturated) {		/* protected */
		gen = page_inc_gen(s, p);
		list_move(&p->lru, &s->gens[gen]);
		if (!saturated)
			s->protected[tier]++;
		return true;
	}
	return false;
}

/* Put a page back after the rmap walk found it accessed. */
static void lru_gen_putback(struct sim *s, struct page *p)
{
	/* never into the oldest generation; the youngest if it is hot */
	int gen = gen_from_seq(s->max_seq - 1 + p->workingset);

	set_gen(s, p, gen);
	list_move(&p->lru, &s->gens[gen]);
}

static struct page *lru_gen_victim(struct sim *s)
{
	for (;;) {
		int tier_idx = get_tier_idx(s);

		if (nr_gens(s) == MIN_NR_GENS)
			lru_gen_age(s);	/* eviction would eat the young */

		struct list_head *head = &s->gens[gen_from_seq(s->min_seq)];

		if (list_empty(head)) {
			try_to_inc_min_seq(s);
			continue;
		}
		struct page *p = lru_to_page(head);

		if (sort_page(s, p, tier_idx))
			continue;
		if (p->accessed) {		/* the rmap walk: still in use */
			p->accessed = false;
			if (p->referenced || p->workingset)
				p->workingset = true;
			p->referenced = false;
			p->refs = 0;
			lru_gen_putback(s, p);
			continue;
		}
		/* evict, leaving the oldest generation number behind */
		s->evicted[tier_of(p)]++;
		p->shadow = pack_shadow(0, 0, (s->min_seq << 2) |
					(page_refs(p) ? page_refs(p) - 1 : 0),
					p->workingset);
		s->nr_gen[p->gen - 1]--;
		p->gen = 0;
		return p;
	}
}

static void lru_gen_add(struct sim *s, struct page *p, bool fd)
{
	unsigned long seq;

	if (p->shadow) {
		unsigned long token;
		int memcgid, nid;
		bool ws;

		unpack_shadow(p->shadow, &memcgid, &nid, &token, &ws);
		/* recent: evicted within the last MAX_NR_GENS generations */
		if (s->max_seq - (token >> 2) < MAX_NR_GENS) {
			int refs = (token & 3) + 1;

			s->refaulted_t[ws ? MAX_NR_TIERS - 1 : order_base_2(refs)]++;
			p->workingset = ws;
			s->st.refault_activations += ws;
		}
		p->shadow = 0;
	}
	if (fd)		/* read(): start old, or one up if in the working set */
		seq = s->max_seq - (MAX_NR_GENS - 1) + p->workingset;
	else		/* a fault: start young, the youngest if hot */
		seq = s->max_seq - (MIN_NR_GENS - 1) + p->workingset;
	if (seq < s->min_seq)
		seq = s->min_seq;
	set_gen(s, p, gen_from_seq(seq));
	list_add(&p->lru, &s->gens[gen_from_seq(seq)]);
	p->accessed = !fd;
	if (fd)
		lru_gen_inc_refs(p);
}

/* ---- OPT (Belady) -------------------------------------------------- */

static void heap_push(struct sim *s, long key, unsigned int id)
{
	long i = s->heap_n++;

	while (i && s->heap[(i - 1) / 2].key < key) {
		s->heap[i] = s->heap[(i - 1) / 2];
		i = (i - 1) / 2;
	}
	s->heap[i] = (struct heap_ent){ key, id };
}

static struct heap_ent heap_pop(struct sim *s)
{
	struct heap_ent top = s->heap[0], last = s->heap[--s->heap_n];
	long i = 0;

	for (;;) {
		long c = 2 * i + 1;

		if (c >= s->heap_n)
			break;
		if (c + 1 < s->heap_n && s->heap[c + 1].key > s->heap[c].key)
			c++;
		if (s->heap[c].key <= last.key)
			break;
		s->heap[i] = s->heap[c];
		i = c;
	}
	if (s->heap_n)
		s->heap[i] = last;
	return top;
}

/* Evict the resident page whose next use is furthest in the future. */
static struct page *opt_victim(struct sim *s)
{
	for (;;) {
		struct heap_ent e = heap_pop(s);
		struct page *p = &s->pg[e.id];

		/* Entries go stale when a page is used again: skip them. */
		if (p->resident && p->next_use == e.key)
			return p;
	}
}

void sim_set_trace(struct sim *s, const uint32_t *trace, long n)
{
	long *last = malloc(MAX_PAGES * sizeof(*last));

	assert(last);
	for (unsigned int i = 0; i < MAX_PAGES; i++)
		last[i] = LONG_MAX;		/* never used again */
	free(s->next);
	free(s->heap);
	s->next = malloc((n ? n : 1) * sizeof(*s->next));
	s->heap = malloc((n + 1) * sizeof(*s->heap));
	assert(s->next && s->heap);
	for (long i = n - 1; i >= 0; i--) {
		uint32_t pg = acc_page(trace[i]);

		s->next[i] = last[pg];
		last[pg] = i;
	}
	s->trace = trace;
	free(last);
}

/* ---- the common driver --------------------------------------------- */

struct sim *sim_new(enum policy pol, int capacity)
{
	struct sim *s = calloc(1, sizeof(*s));

	assert(s && capacity > 0);
	s->pol = pol;
	s->cap = capacity;
	s->pg = calloc(MAX_PAGES, sizeof(*s->pg));
	assert(s->pg);
	for (unsigned int i = 0; i < MAX_PAGES; i++)
		list_init(&s->pg[i].lru);
	list_init(&s->lists[0]);
	list_init(&s->lists[1]);
	s->inactive = &s->lists[0];
	s->active = &s->lists[1];
	for (int g = 0; g < MAX_NR_GENS; g++)
		list_init(&s->gens[g]);
	s->max_seq = MAX_NR_GENS;	/* any start works; avoid underflow */
	s->min_seq = s->max_seq - MIN_NR_GENS + 1;
	return s;
}

void sim_free(struct sim *s)
{
	free(s->pg);
	free(s->next);
	free(s->heap);
	free(s);
}

const struct sim_stats *sim_stats(const struct sim *s)
{
	return &s->st;
}

static struct page *pick_victim(struct sim *s)
{
	switch (s->pol) {
	case POL_LRU:
	case POL_CLOCK:
		return lru_victim(s);
	case POL_2LIST:
	case POL_2LIST_WS:
		return two_list_victim(s);
	case POL_MGLRU:
		return lru_gen_victim(s);
	default:
		return opt_victim(s);
	}
}

bool sim_access(struct sim *s, const uint32_t *trace, long n, long i)
{
	uint32_t a = trace[i];
	struct page *p = &s->pg[acc_page(a)];
	bool fd = acc_fd(a);

	assert(acc_page(a) < MAX_PAGES && i < n);
	if (s->pol == POL_OPT) {
		assert(s->trace == trace);
		p->next_use = s->next[i];
	}
	if (p->resident) {
		s->st.hits++;
		switch (s->pol) {
		case POL_LRU:
		case POL_CLOCK:
			lru_hit(s, p);
			break;
		case POL_2LIST:
		case POL_2LIST_WS:
			two_list_hit(s, p, fd);
			break;
		case POL_MGLRU:
			if (fd)
				lru_gen_inc_refs(p);
			else
				p->accessed = true;
			break;
		default:
			heap_push(s, p->next_use, acc_page(a));
		}
		return true;
	}

	s->st.misses++;
	if (s->nr_resident == s->cap) {
		struct page *v = pick_victim(s);

		list_del(&v->lru);
		v->resident = v->accessed = v->referenced = false;
		v->active = v->workingset = false;
		v->refs = 0;
		s->nr_resident--;
		s->st.evictions++;
	}
	p->resident = true;
	s->nr_resident++;
	switch (s->pol) {
	case POL_LRU:
	case POL_CLOCK:
		list_add(&p->lru, &s->lists[0]);
		break;
	case POL_2LIST:
	case POL_2LIST_WS:
		two_list_add(s, p, fd);
		break;
	case POL_MGLRU:
		lru_gen_add(s, p, fd);
		break;
	default:
		list_add(&p->lru, &s->lists[0]);	/* only for sim_check */
		heap_push(s, p->next_use, acc_page(a));
	}
	return false;
}

long sim_run(enum policy pol, int capacity, const uint32_t *trace, long n,
	     struct sim_stats *st)
{
	struct sim *s = sim_new(pol, capacity);

	if (pol == POL_OPT)
		sim_set_trace(s, trace, n);
	for (long i = 0; i < n; i++)
		sim_access(s, trace, n, i);
	if (st)
		*st = s->st;
	long hits = s->st.hits;

	sim_free(s);
	return hits;
}

/* ---- invariants ---------------------------------------------------- */

static long count_list(const struct list_head *head)
{
	long n = 0;

	for (const struct list_head *x = head->next; x != head; x = x->next)
		n++;
	return n;
}

void sim_check(const struct sim *s)
{
	long on_lists = 0;

	assert(s->nr_resident <= s->cap);
	assert(s->st.hits + s->st.misses == s->st.evictions + s->nr_resident +
	       s->st.hits);
	switch (s->pol) {
	case POL_LRU:
	case POL_CLOCK:
	case POL_OPT:
		on_lists = count_list(&s->lists[0]);
		break;
	case POL_2LIST:
	case POL_2LIST_WS: {
		const struct page *p;

		assert(count_list(s->inactive) == s->nr_inactive);
		assert(count_list(s->active) == s->nr_active);
		list_for_each_entry(p, s->inactive, lru)
			assert(p->resident && !p->active);
		list_for_each_entry(p, s->active, lru)
			assert(p->resident && p->active);
		on_lists = s->nr_inactive + s->nr_active;
		break;
	}
	case POL_MGLRU: {
		long by_gen[MAX_NR_GENS] = { 0 };

		assert(nr_gens(s) >= MIN_NR_GENS && nr_gens(s) <= MAX_NR_GENS);
		for (int g = 0; g < MAX_NR_GENS; g++) {
			const struct page *p;
			/* is list g inside the window [min_seq, max_seq]? */
			bool live = (unsigned long)((g - gen_from_seq(s->min_seq) +
				     MAX_NR_GENS) % MAX_NR_GENS) < (unsigned long)nr_gens(s);

			assert(live || list_empty(&s->gens[g]));
			list_for_each_entry(p, &s->gens[g], lru) {
				int pg = p->gen - 1, d;

				assert(p->resident && p->gen);
				/* a page may only be promoted, never demoted,
				 * relative to the list it sits on */
				d = (pg - gen_from_seq(s->min_seq) + MAX_NR_GENS) %
				    MAX_NR_GENS;
				assert(d < nr_gens(s));
				by_gen[pg]++;
				on_lists++;
			}
		}
		for (int g = 0; g < MAX_NR_GENS; g++)
			assert(by_gen[g] == s->nr_gen[g]);
		break;
	}
	default:
		abort();
	}
	assert(on_lists == s->nr_resident);
}
