// SPDX-License-Identifier: MIT
/*
 * wheel.c - the teaching timer wheel; see wheel.h for what it covers.
 */
#include "wheel.h"

void wheel_init(struct wheel *w, unsigned long now)
{
	w->clk = now;
	w->next_expiry = now + NEXT_MAX_DELTA;
	w->next_expiry_recalc = false;
	w->timers_pending = false;
	bitmap_zero(w->pending_map, WHEEL_SIZE);
	for (unsigned long i = 0; i < WHEEL_SIZE; i++)
		w->vectors[i].first = NULL;
}

void wtimer_init(struct wtimer *t, void (*fn)(struct wtimer *, unsigned long))
{
	INIT_HLIST_NODE(&t->entry);
	t->expires = 0;
	t->idx = 0;
	t->fn = fn;
}

/* ------------------------------------------------------------------ */
/* Bucketing */

static unsigned int calc_index(unsigned long expires, unsigned int lvl,
			       unsigned long *bucket_expiry)
{
	/*
	 * Round up to the level's granularity: the bucket expires at the
	 * first multiple of LVL_GRAN(lvl) after expires, so the timer can be
	 * late by up to one granule but is never early.
	 */
	expires = (expires >> LVL_SHIFT(lvl)) + 1;
	*bucket_expiry = expires << LVL_SHIFT(lvl);
	return LVL_OFFS(lvl) + (expires & LVL_MASK);
}

unsigned int calc_wheel_index(unsigned long expires, unsigned long clk,
			      unsigned long *bucket_expiry)
{
	unsigned long delta = expires - clk;
	unsigned int lvl;

	if ((long)delta < 0) {			/* already due: next tick */
		*bucket_expiry = clk;
		return clk & LVL_MASK;
	}
	if (delta >= WHEEL_TIMEOUT_CUTOFF)	/* beyond the last level */
		expires = clk + WHEEL_TIMEOUT_MAX;

	for (lvl = 0; lvl < LVL_DEPTH - 1; lvl++)
		if (delta < LVL_START(lvl + 1))
			break;
	return calc_index(expires, lvl, bucket_expiry);
}

/* ------------------------------------------------------------------ */
/* Adding, modifying, cancelling */

static void enqueue_timer(struct wheel *w, struct wtimer *t, unsigned int idx,
			  unsigned long bucket_expiry)
{
	hlist_add_head(&t->entry, &w->vectors[idx]);
	__set_bit(idx, w->pending_map);
	t->idx = idx;

	/* The bucket's expiry, not t->expires, is when t will really fire. */
	if (time_before(bucket_expiry, w->next_expiry)) {
		w->next_expiry = bucket_expiry;
		w->timers_pending = true;
		w->next_expiry_recalc = false;
	}
}

static bool hlist_is_singular_node(const struct hlist_node *n,
				   const struct hlist_head *h)
{
	return !n->next && n->pprev == &h->first;
}

static int detach_if_pending(struct wheel *w, struct wtimer *t)
{
	if (!wtimer_pending(t))
		return 0;

	/* Last timer in its bucket: clear its bit, recompute lazily. */
	if (t->idx != NO_BUCKET &&
	    hlist_is_singular_node(&t->entry, &w->vectors[t->idx])) {
		__clear_bit(t->idx, w->pending_map);
		w->next_expiry_recalc = true;
	}
	hlist_del_init(&t->entry);
	return 1;
}

/* Move clk up to now, but never past a bucket that is already due. */
static void forward_clk(struct wheel *w, unsigned long now)
{
	if (time_before_eq(now, w->clk))
		return;
	if (time_after(w->next_expiry, now))
		w->clk = now;
	else
		w->clk = w->next_expiry;
}

int wheel_mod(struct wheel *w, struct wtimer *t, unsigned long expires,
	      unsigned long now)
{
	unsigned long bucket_expiry;
	unsigned int idx;
	int ret;

	if (wtimer_pending(t) && t->expires == expires)
		return 1;

	forward_clk(w, now);
	idx = calc_wheel_index(expires, w->clk, &bucket_expiry);

	/* Still the same bucket: nothing to move (a networking favourite). */
	if (wtimer_pending(t) && idx == t->idx) {
		t->expires = expires;
		return 1;
	}

	ret = detach_if_pending(w, t);
	t->expires = expires;
	enqueue_timer(w, t, idx, bucket_expiry);
	return ret;
}

int wheel_cancel(struct wheel *w, struct wtimer *t)
{
	return detach_if_pending(w, t);
}

/* ------------------------------------------------------------------ */
/* Expiry */

static void hlist_move_list(struct hlist_head *old, struct hlist_head *new)
{
	new->first = old->first;
	if (new->first)
		new->first->pprev = &new->first;
	old->first = NULL;
}

static int collect_expired_timers(struct wheel *w, struct hlist_head *heads)
{
	unsigned long clk = w->clk = w->next_expiry;
	int levels = 0;

	for (unsigned int lvl = 0; lvl < LVL_DEPTH; lvl++) {
		unsigned int idx = LVL_OFFS(lvl) + (clk & LVL_MASK);

		if (test_bit(idx, w->pending_map)) {
			__clear_bit(idx, w->pending_map);
			hlist_move_list(&w->vectors[idx], &heads[levels++]);
		}
		/* The next level is due only when this one's clock wraps. */
		if (clk & LVL_CLK_MASK)
			break;
		clk >>= LVL_CLK_SHIFT;
	}
	return levels;
}

/*
 * Distance from bucket clk to the next pending bucket of one level,
 * wrapping around the end of the level; -1 if the level is empty.
 */
static int next_pending_bucket(struct wheel *w, unsigned int offset,
			       unsigned int clk)
{
	unsigned int pos, start = offset + clk;
	unsigned int end = offset + LVL_SIZE;

	pos = find_next_bit(w->pending_map, end, start);
	if (pos < end)
		return pos - start;

	pos = find_next_bit(w->pending_map, start, offset);
	return pos < start ? (int)(pos + LVL_SIZE - start) : -1;
}

static void recalc_next_expiry(struct wheel *w)
{
	unsigned long clk = w->clk, next = w->clk + NEXT_MAX_DELTA;
	unsigned int lvl, offset = 0;

	for (lvl = 0; lvl < LVL_DEPTH; lvl++, offset += LVL_SIZE) {
		int pos = next_pending_bucket(w, offset, clk & LVL_MASK);
		unsigned long lvl_clk = clk & LVL_CLK_MASK;

		if (pos >= 0) {
			unsigned long tmp = (clk + pos) << LVL_SHIFT(lvl);

			if (time_before(tmp, next))
				next = tmp;
			/* Found before the next level's clock even ticks? Done. */
			if ((unsigned long)pos <= ((LVL_CLK_DIV - lvl_clk) & LVL_CLK_MASK))
				break;
		}
		/* Next level's clock; round up if this level is mid-cycle. */
		clk >>= LVL_CLK_SHIFT;
		clk += lvl_clk ? 1 : 0;
	}
	w->next_expiry = next;
	w->next_expiry_recalc = false;
	w->timers_pending = next != w->clk + NEXT_MAX_DELTA;
}

unsigned long wheel_next_expiry(struct wheel *w)
{
	if (w->next_expiry_recalc)
		recalc_next_expiry(w);
	if (!w->timers_pending)			/* keep "never" in the future */
		w->next_expiry = w->clk + NEXT_MAX_DELTA;
	return w->next_expiry;
}

#ifndef NO_EXPIRING_MARK
/*
 * Timers taken out of their buckets for expiry are still pending until
 * their callback runs.  Mark them so that wheel_mod() from another
 * callback cannot mistake one for a timer still in its bucket (the kernel
 * keeps no such mark; see the notes for this chapter).
 */
static void mark_expiring(struct hlist_head *head)
{
	struct wtimer *t;

	hlist_for_each_entry(t, head, entry)
		t->idx = NO_BUCKET;
}
#endif

static unsigned long expire_timers(struct hlist_head *head, unsigned long tick)
{
	unsigned long n = 0;

	while (!hlist_empty(head)) {
		struct wtimer *t = container_of(head->first, struct wtimer, entry);

		hlist_del_init(&t->entry);
		t->fn(t, tick);			/* may re-arm t or touch others */
		n++;
	}
	return n;
}

unsigned long wheel_run(struct wheel *w, unsigned long now)
{
	struct hlist_head heads[LVL_DEPTH];
	unsigned long fired = 0;

	while (time_after_eq(now, w->clk) && time_after_eq(now, w->next_expiry)) {
		int levels = collect_expired_timers(w, heads);
		unsigned long tick = w->clk;

		w->clk++;		/* one ahead while the callbacks run */
		recalc_next_expiry(w);
#ifndef NO_EXPIRING_MARK
		for (int i = 0; i < levels; i++)
			mark_expiring(&heads[i]);
#endif
		while (levels--)
			fired += expire_timers(&heads[levels], tick);
	}
	return fired;
}
