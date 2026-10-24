// SPDX-License-Identifier: MIT
/*
 * bn.c - big-integer arithmetic for Chapter 12; see bn.h.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bn.h"

struct bn_stats bn_stats;
int karatsuba_threshold = KARATSUBA_THRESHOLD;

/* ------------------------------------------------- raw limb arrays */

limb_t mpn_add_n(limb_t *r, const limb_t *a, const limb_t *b, int n)
{
	limb_t carry = 0;

	for (int i = 0; i < n; i++) {
		dlimb_t t = (dlimb_t)a[i] + b[i] + carry;

		r[i] = (limb_t)t;
		carry = (limb_t)(t >> LIMB_BITS);
	}
	return carry;
}

limb_t mpn_sub_n(limb_t *r, const limb_t *a, const limb_t *b, int n)
{
	limb_t borrow = 0;

	for (int i = 0; i < n; i++) {
		limb_t ai = a[i], bi = b[i];

		r[i] = ai - bi - borrow;
		borrow = (ai < bi) || (ai == bi && borrow);
	}
	return borrow;
}

/* r = a + v (n limbs); returns the carry out. */
limb_t mpn_add_1(limb_t *r, const limb_t *a, int n, limb_t v)
{
	for (int i = 0; i < n; i++) {
		limb_t t = a[i] + v;

		v = t < v;
		r[i] = t;
	}
	return v;
}

int mpn_cmp(const limb_t *a, const limb_t *b, int n)
{
	while (n--)
		if (a[n] != b[n])
			return a[n] > b[n] ? 1 : -1;
	return 0;
}

/* r[0..n) += a[0..n) * v; returns the limb that carries out. */
limb_t mpn_addmul_1(limb_t *r, const limb_t *a, int n, limb_t v)
{
	limb_t carry = 0;

	for (int i = 0; i < n; i++) {
		dlimb_t t = (dlimb_t)a[i] * v + r[i] + carry;

		r[i] = (limb_t)t;
		carry = (limb_t)(t >> LIMB_BITS);
	}
	bn_stats.limb_muls += n;
	return carry;
}

/* r[0..n) -= a[0..n) * v; returns the limb that borrows out. */
static limb_t mpn_submul_1(limb_t *r, const limb_t *a, int n, limb_t v)
{
	limb_t borrow = 0;

	for (int i = 0; i < n; i++) {
		dlimb_t p = (dlimb_t)a[i] * v + borrow;
		limb_t lo = (limb_t)p, ri = r[i];

		r[i] = ri - lo;
		borrow = (limb_t)(p >> LIMB_BITS) + (ri < lo);
	}
	bn_stats.limb_muls += n;
	return borrow;
}

/* Schoolbook: one row of partial products per limb of b. */
void mpn_mul_basecase(limb_t *r, const limb_t *a, int an,
		      const limb_t *b, int bn)
{
	memset(r, 0, an * sizeof(*r));
	for (int i = 0; i < bn; i++)
		r[an + i] = mpn_addmul_1(r + i, a, an, b[i]);
}

/*
 * r[0..2n) = a[0..n) * b[0..n).  Karatsuba, with a0, a1 the low and high
 * halves of a (h limbs each) and B = 2^32:
 *
 *   a b = B^2h H + B^h (L + H + (a1 - a0)(b0 - b1)) + L,
 *   L = a0 b0,  H = a1 b1.
 *
 * tmp needs 4n + 64 limbs.
 */
void mpn_mul_n(limb_t *r, const limb_t *a, const limb_t *b, int n,
	       limb_t *tmp)
{
	if (n < karatsuba_threshold) {
		mpn_mul_basecase(r, a, n, b, n);
		return;
	}
	if (n & 1) {
		/* Odd size: Karatsuba on n - 1 limbs, then the top limbs. */
		int e = n - 1;

		mpn_mul_n(r, a, b, e, tmp);
		r[e + e] = mpn_addmul_1(r + e, a, e, b[e]);
		r[e + n] = mpn_addmul_1(r + e, b, n, a[e]);
		return;
	}

	int h = n / 2;
	limb_t *da = tmp, *db = tmp + h, *m = tmp + n, *t = tmp + 2 * n;
	bool neg = false;

	mpn_mul_n(r, a, b, h, tmp);			/* L, low half of r */
	mpn_mul_n(r + n, a + h, b + h, h, tmp);		/* H, high half */

	/* M = |a1 - a0| * |b0 - b1|, remembering the sign of the product */
	if (mpn_cmp(a + h, a, h) >= 0) {
		mpn_sub_n(da, a + h, a, h);
	} else {
		mpn_sub_n(da, a, a + h, h);
		neg = !neg;
	}
	if (mpn_cmp(b, b + h, h) >= 0) {
		mpn_sub_n(db, b, b + h, h);
	} else {
		mpn_sub_n(db, b + h, b, h);
		neg = !neg;
	}
	mpn_mul_n(m, da, db, h, tmp + 2 * n);

	/* t = L + H +/- M = a0 b1 + a1 b0, which is never negative */
	t[n] = mpn_add_n(t, r, r + n, n);
	if (neg)
		t[n] -= mpn_sub_n(t, t, m, n);
	else
		t[n] += mpn_add_n(t, t, m, n);

	/* r += t * B^h */
	limb_t c = mpn_add_n(r + h, r + h, t, n + 1);

	mpn_add_1(r + n + h + 1, r + n + h + 1, n - h - 1, c);
}

/*
 * r[0..an+bn) = a * b, an >= bn >= 1.  Like the kernel, cut the longer
 * operand into pieces as long as the shorter one, so that every
 * Karatsuba call multiplies two numbers of the same size.
 */
void mpn_mul(limb_t *r, const limb_t *a, int an, const limb_t *b, int bn)
{
	limb_t tmp[4 * BN_MAX + 64], tp[2 * BN_MAX];

	if (bn < karatsuba_threshold) {
		mpn_mul_basecase(r, a, an, b, bn);
		return;
	}
	mpn_mul_n(r, a, b, bn, tmp);
	r += bn;
	a += bn;
	an -= bn;
	while (an >= bn) {
		mpn_mul_n(tp, a, b, bn, tmp);
		limb_t c = mpn_add_n(r, r, tp, bn);

		mpn_add_1(r + bn, tp + bn, bn, c);
		r += bn;
		a += bn;
		an -= bn;
	}
	if (an) {
		mpn_mul(tp, b, bn, a, an);
		limb_t c = mpn_add_n(r, r, tp, bn);

		mpn_add_1(r + bn, tp + bn, an, c);
	}
}

/* Shift n limbs left by s bits (0 <= s < 32), returning the bits shifted out. */
static limb_t lshift(limb_t *r, const limb_t *a, int n, int s)
{
	limb_t out = s ? a[n - 1] >> (LIMB_BITS - s) : 0;

	for (int i = n - 1; i > 0; i--)
		r[i] = s ? (a[i] << s) | (a[i - 1] >> (LIMB_BITS - s)) : a[i];
	r[0] = a[0] << s;
	return out;
}

/*
 * Long division (Knuth's Algorithm D): q[0..un-vn] = u / v and
 * r[0..vn) = u % v, for un >= vn >= 1 and v[vn - 1] != 0.  q may be NULL.
 *
 * The divisor is first shifted so that its top bit is set.  Then the
 * estimate of each quotient limb from the top two limbs of the
 * remainder and the top limb of the divisor is at most 2 too large.
 */
void mpn_divrem(limb_t *q, limb_t *r, const limb_t *u, int un,
		const limb_t *v, int vn)
{
	limb_t vv[BN_MAX], uu[BN_MAX + 1];

	assert(un >= vn && vn >= 1 && un <= BN_MAX && v[vn - 1]);
	if (vn == 1) {
		dlimb_t rem = 0;

		for (int j = un - 1; j >= 0; j--) {
			dlimb_t t = rem << LIMB_BITS | u[j];

			if (q)
				q[j] = (limb_t)(t / v[0]);
			rem = t % v[0];
		}
		r[0] = (limb_t)rem;
		return;
	}

	int s = __builtin_clz(v[vn - 1]);	/* normalize */

	lshift(vv, v, vn, s);
	uu[un] = lshift(uu, u, un, s);

	for (int j = un - vn; j >= 0; j--) {
		dlimb_t num = (dlimb_t)uu[j + vn] << LIMB_BITS | uu[j + vn - 1];
		dlimb_t qhat = num / vv[vn - 1], rhat = num % vv[vn - 1];

		/* Refine the estimate with the second divisor limb. */
		while (qhat >> LIMB_BITS ||
		       qhat * vv[vn - 2] > (rhat << LIMB_BITS | uu[j + vn - 2])) {
			qhat--;
			rhat += vv[vn - 1];
			if (rhat >> LIMB_BITS)
				break;
		}

		limb_t borrow = mpn_submul_1(uu + j, vv, vn, (limb_t)qhat);
		limb_t top = uu[j + vn];

		uu[j + vn] = top - borrow;
		if (top < borrow) {		/* still one too large: add back */
			qhat--;
			uu[j + vn] += mpn_add_n(uu + j, uu + j, vv, vn);
		}
		if (q)
			q[j] = (limb_t)qhat;
	}

	for (int i = 0; i < vn; i++)		/* unnormalize the remainder */
		r[i] = s ? (uu[i] >> s) | (uu[i + 1] << (LIMB_BITS - s)) : uu[i];
}

/* ------------------------------------------------------- struct bn */

void bn_norm(struct bn *a)
{
	while (a->n > 0 && !a->d[a->n - 1])
		a->n--;
}

void bn_set_u64(struct bn *r, uint64_t v)
{
	r->d[0] = (limb_t)v;
	r->d[1] = (limb_t)(v >> LIMB_BITS);
	r->n = 2;
	bn_norm(r);
}

uint64_t bn_get_u64(const struct bn *a)
{
	uint64_t v = 0;

	for (int i = a->n - 1; i >= 0; i--)
		v = v << LIMB_BITS | a->d[i];
	return v;
}

int bn_cmp(const struct bn *a, const struct bn *b)
{
	if (a->n != b->n)
		return a->n > b->n ? 1 : -1;
	return mpn_cmp(a->d, b->d, a->n);
}

int bn_bits(const struct bn *a)
{
	if (!a->n)
		return 0;
	return a->n * LIMB_BITS - __builtin_clz(a->d[a->n - 1]);
}

bool bn_bit(const struct bn *a, int i)
{
	if (i / LIMB_BITS >= a->n)
		return false;
	return a->d[i / LIMB_BITS] >> (i % LIMB_BITS) & 1;
}

void bn_add(struct bn *r, const struct bn *a, const struct bn *b)
{
	int n = a->n > b->n ? a->n : b->n;
	limb_t carry = 0;

	assert(n < BN_MAX);
	for (int i = 0; i < n; i++) {
		dlimb_t t = (dlimb_t)carry;

		t += i < a->n ? a->d[i] : 0;
		t += i < b->n ? b->d[i] : 0;
		r->d[i] = (limb_t)t;
		carry = (limb_t)(t >> LIMB_BITS);
	}
	r->d[n] = carry;
	r->n = n + 1;
	bn_norm(r);
}

/* r = a - b, a >= b */
void bn_sub(struct bn *r, const struct bn *a, const struct bn *b)
{
	limb_t borrow = 0;

	assert(bn_cmp(a, b) >= 0);
	for (int i = 0; i < a->n; i++) {
		limb_t ai = a->d[i], bi = i < b->n ? b->d[i] : 0;

		r->d[i] = ai - bi - borrow;
		borrow = (ai < bi) || (ai == bi && borrow);
	}
	r->n = a->n;
	bn_norm(r);
}

void bn_mul(struct bn *r, const struct bn *a, const struct bn *b)
{
	struct bn t;

	if (!a->n || !b->n) {
		r->n = 0;
		return;
	}
	if (a->n < b->n) {
		const struct bn *x = a;

		a = b;
		b = x;
	}
	assert(a->n + b->n <= BN_MAX);
	mpn_mul(t.d, a->d, a->n, b->d, b->n);
	t.n = a->n + b->n;
	bn_norm(&t);
	*r = t;
}

/* q = a / m, r = a % m; either may be NULL. */
void bn_divrem(struct bn *q, struct bn *r, const struct bn *a,
	       const struct bn *m)
{
	struct bn tq, tr;

	assert(m->n);
	if (bn_cmp(a, m) < 0) {
		tq.n = 0;
		tr = *a;
	} else {
		mpn_divrem(tq.d, tr.d, a->d, a->n, m->d, m->n);
		tq.n = a->n - m->n + 1;
		tr.n = m->n;
		bn_norm(&tq);
		bn_norm(&tr);
	}
	if (q)
		*q = tq;
	if (r)
		*r = tr;
}

limb_t bn_mod_1(const struct bn *a, limb_t m)
{
	dlimb_t rem = 0;

	for (int i = a->n - 1; i >= 0; i--)
		rem = (rem << LIMB_BITS | a->d[i]) % m;
	return (limb_t)rem;
}

void bn_mod(struct bn *r, const struct bn *a, const struct bn *m)
{
	bn_divrem(NULL, r, a, m);
}

void bn_mulm(struct bn *r, const struct bn *a, const struct bn *b,
	     const struct bn *m)
{
	bn_stats.mul++;
	bn_mul(r, a, b);
	bn_mod(r, r, m);
}

void bn_sqrm(struct bn *r, const struct bn *a, const struct bn *m)
{
	bn_stats.sqr++;
	bn_mul(r, a, a);
	bn_mod(r, r, m);
}

/* ------------------------------------------- modular exponentiation */

/* r = base^e mod m: left-to-right square-and-multiply, as in mpi_powm(). */
void bn_powm(struct bn *r, const struct bn *base, const struct bn *e,
	     const struct bn *m)
{
	struct bn b, x;

	if (!e->n) {			/* x^0 = 1, or 0 when m = 1 */
		bn_set_u64(&x, 1);
		bn_mod(r, &x, m);
		return;
	}
	bn_mod(&b, base, m);
	x = b;				/* the top bit of e is 1 */
	for (int i = bn_bits(e) - 2; i >= 0; i--) {
		bn_sqrm(&x, &x, m);
		if (bn_bit(e, i))
			bn_mulm(&x, &x, &b, m);
	}
	*r = x;
}

/*
 * Sliding window: precompute the odd powers b, b^3, ..., b^(2^k - 1);
 * then each run of up to k bits that starts and ends with a 1 costs one
 * multiplication.  Zero bits between windows cost only squarings.
 */
void bn_powm_window(struct bn *r, const struct bn *base, const struct bn *e,
		    const struct bn *m, int k)
{
	static struct bn g[1 << 6];	/* g[i] = b^(2i + 1) */
	struct bn b2, x;
	bool started = false;

	assert(k >= 1 && k <= 7);
	bn_set_u64(&x, 1);
	bn_mod(&x, &x, m);		/* the result if e = 0 */
	bn_mod(&g[0], base, m);
	bn_sqrm(&b2, &g[0], m);
	for (int i = 1; i < 1 << (k - 1); i++)
		bn_mulm(&g[i], &g[i - 1], &b2, m);

	for (int i = bn_bits(e) - 1; i >= 0; ) {
		if (!bn_bit(e, i)) {
			if (started)
				bn_sqrm(&x, &x, m);
			i--;
			continue;
		}
		/* The window is e[i..l]: at most k bits, ending in a 1. */
		int l = i - k + 1 < 0 ? 0 : i - k + 1;
		unsigned int w = 0;

		while (!bn_bit(e, l))
			l++;
		for (int j = i; j >= l; j--)
			w = w << 1 | bn_bit(e, j);
		if (started) {
			for (int j = i; j >= l; j--)
				bn_sqrm(&x, &x, m);
			bn_mulm(&x, &x, &g[w >> 1], m);
		} else {
			x = g[w >> 1];
			started = true;
		}
		i = l - 1;
	}
	*r = x;
}

static void zero_above(struct bn *a, int n)
{
	for (int i = a->n; i < n; i++)
		a->d[i] = 0;
}

/* Swap a and b if bit is 1, without a branch on bit. */
void bn_cswap(struct bn *a, struct bn *b, limb_t bit)
{
	limb_t mask = (limb_t)0 - bit;		/* all ones or all zeros */

	for (int i = 0; i < BN_MAX; i++) {
		limb_t t = (a->d[i] ^ b->d[i]) & mask;

		a->d[i] ^= t;
		b->d[i] ^= t;
	}
	int tn = (a->n ^ b->n) & (int)mask;

	a->n ^= tn;
	b->n ^= tn;
}

/*
 * Montgomery ladder over the low nbits bits of e: the same sequence of
 * one product and one squaring for every bit, whatever its value.
 * Keeps R1 = R0 * b.
 */
void bn_powm_ladder(struct bn *r, const struct bn *base, const struct bn *e,
		    const struct bn *m, int nbits)
{
	static struct bn r0, r1;

	bn_set_u64(&r0, 1);
	bn_mod(&r0, &r0, m);
	bn_mod(&r1, base, m);
	for (int i = nbits - 1; i >= 0; i--) {
		limb_t bit = bn_bit(e, i);

		zero_above(&r0, BN_MAX);	/* cswap reads every limb */
		zero_above(&r1, BN_MAX);
		bn_cswap(&r0, &r1, bit);
		bn_mulm(&r1, &r0, &r1, m);
		bn_sqrm(&r0, &r0, m);
		zero_above(&r0, BN_MAX);
		zero_above(&r1, BN_MAX);
		bn_cswap(&r0, &r1, bit);
	}
	*r = r0;
}

void bn_to_hex(const struct bn *a, char *buf)
{
	if (!a->n) {
		strcpy(buf, "0");
		return;
	}
	buf += sprintf(buf, "%x", a->d[a->n - 1]);
	for (int i = a->n - 2; i >= 0; i--)
		buf += sprintf(buf, "%08x", a->d[i]);
}
