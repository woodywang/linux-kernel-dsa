// SPDX-License-Identifier: MIT
/*
 * test_bn.c - randomized tests for bn.c and rsa.c.
 *
 *   ./test_bn [iterations] [seed]
 *
 * Multiplication: Karatsuba (with a random threshold) against
 * schoolbook, for random operand sizes and limb patterns.  Division:
 * q * v + r = u and r < v, and against 64-bit arithmetic for small
 * operands.  Exponentiation: left-to-right, sliding window (k = 1..6)
 * and the Montgomery ladder against a right-to-left reference, and
 * against 128-bit arithmetic for moduli below 2^64.  RSA: for keys of
 * 64 to 512 bits made from a deterministic seed, CRT decryption must
 * invert encryption and agree with direct decryption.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bn.h"
#include "rsa.h"

typedef unsigned __int128 u128;

static unsigned long long rng_state;

static uint64_t rnd(void)
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return rng_state * 2685821657736338717ULL;
}

/* Random limbs, sometimes all ones or zeros, to provoke carries. */
static void rnd_limbs(limb_t *a, int n)
{
	int mode = rnd() % 4;

	for (int i = 0; i < n; i++) {
		switch (mode) {
		case 0: a[i] = 0xffffffff; break;
		case 1: a[i] = rnd() % 3 ? 0xffffffff : (limb_t)rnd(); break;
		case 2: a[i] = rnd() % 3 ? 0 : (limb_t)rnd(); break;
		default: a[i] = (limb_t)rnd(); break;
		}
	}
}

static void rnd_bn(struct bn *a, int maxlimbs)
{
	a->n = 1 + rnd() % maxlimbs;
	rnd_limbs(a->d, a->n);
	if (rnd() % 2)
		a->d[a->n - 1] >>= rnd() % 32;
	bn_norm(a);
}

static void test_mul(void)
{
	static limb_t a[BN_MAX], b[BN_MAX], r1[2 * BN_MAX], r2[2 * BN_MAX];
	static limb_t tmp[4 * BN_MAX + 64];
	int an = 1 + rnd() % 80, bn = 1 + rnd() % an;

	if (an + bn > BN_MAX)
		bn = BN_MAX - an;
	rnd_limbs(a, an);
	rnd_limbs(b, bn);
	karatsuba_threshold = 2 + rnd() % 19;

	mpn_mul_basecase(r1, a, an, b, bn);
	mpn_mul(r2, a, an, b, bn);
	assert(!memcmp(r1, r2, (an + bn) * sizeof(limb_t)));

	mpn_mul_basecase(r1, a, an, a, an);	/* equal sizes, squares */
	mpn_mul_n(r2, a, a, an, tmp);
	assert(!memcmp(r1, r2, 2 * an * sizeof(limb_t)));
	mpn_mul_basecase(r1, a, bn, b, bn);
	mpn_mul_n(r2, a, b, bn, tmp);
	assert(!memcmp(r1, r2, 2 * bn * sizeof(limb_t)));
	karatsuba_threshold = KARATSUBA_THRESHOLD;
}

static void test_div(void)
{
	struct bn u, v, q, r, t;

	rnd_bn(&u, BN_MAX - 1);
	do {
		rnd_bn(&v, rnd() % 4 ? u.n + 1 : 3);
	} while (!v.n);
	bn_divrem(&q, &r, &u, &v);
	assert(bn_cmp(&r, &v) < 0);
	bn_mul(&t, &q, &v);
	bn_add(&t, &t, &r);
	assert(!bn_cmp(&t, &u));
	if (u.n <= 2 && v.n <= 2) {
		assert(bn_get_u64(&q) == bn_get_u64(&u) / bn_get_u64(&v));
		assert(bn_get_u64(&r) == bn_get_u64(&u) % bn_get_u64(&v));
	}
	assert(bn_mod_1(&u, v.d[0] | 1) == (bn_set_u64(&t, v.d[0] | 1),
					     bn_mod(&t, &u, &t), bn_get_u64(&t)));
}

/* Reference: right-to-left binary exponentiation. */
static void powm_rtl(struct bn *r, const struct bn *b, const struct bn *e,
		     const struct bn *m)
{
	struct bn x, s;

	bn_set_u64(&x, 1);
	bn_mod(&x, &x, m);
	bn_mod(&s, b, m);
	for (int i = 0; i < bn_bits(e); i++) {
		if (bn_bit(e, i))
			bn_mulm(&x, &x, &s, m);
		bn_mulm(&s, &s, &s, m);
	}
	*r = x;
}

static uint64_t powm_u64(uint64_t b, uint64_t e, uint64_t m)
{
	u128 x = 1 % m, s = b % m;

	for (; e; e >>= 1) {
		if (e & 1)
			x = x * s % m;
		s = s * s % m;
	}
	return (uint64_t)x;
}

static void test_powm(void)
{
	struct bn b, e, m, r1, r2;
	bool small = rnd() % 2;

	rnd_bn(&m, small ? 2 : 24);
	if (!m.n)
		bn_set_u64(&m, 1 + rnd() % 3);
	rnd_bn(&b, small ? 2 : 30);
	rnd_bn(&e, small ? 2 : 6);
	if (rnd() % 10 == 0)
		e.n = 0;

	bn_powm(&r1, &b, &e, &m);
	powm_rtl(&r2, &b, &e, &m);
	assert(!bn_cmp(&r1, &r2));
	if (small)
		assert(bn_get_u64(&r1) == powm_u64(bn_get_u64(&b), bn_get_u64(&e),
						   bn_get_u64(&m)));
	bn_powm_window(&r2, &b, &e, &m, 1 + rnd() % 6);
	assert(!bn_cmp(&r1, &r2));
	bn_powm_ladder(&r2, &b, &e, &m, bn_bits(&e) + rnd() % 40);
	assert(!bn_cmp(&r1, &r2));
}

static void test_rsa(void)
{
	static struct rsa_key k;
	struct bn m, c, m1, m2;
	int bits = 64 + rnd() % 449;

	rsa_keygen(&k, bits, rnd);
	assert(bn_bits(&k.n) == bits);
	for (int i = 0; i < 8; i++) {
		rnd_bn(&m, k.n.n);
		bn_mod(&m, &m, &k.n);
		if (rsa_enc(&k, &c, &m))	/* m <= 1 or m >= n - 1 */
			continue;
		assert(!rsa_dec_crt(&k, &m1, &c));
		assert(!rsa_dec_direct(&k, &m2, &c));
		assert(!bn_cmp(&m1, &m) && !bn_cmp(&m2, &m));
	}
}

int main(int argc, char **argv)
{
	unsigned long iters = argc > 1 ? strtoul(argv[1], NULL, 0) : 20000;

	rng_state = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	if (!rng_state)
		rng_state = 1;
	for (unsigned long i = 0; i < iters; i++) {
		test_mul();
		test_div();
		if (i % 4 == 0)
			test_powm();
		if (i % 400 == 0)
			test_rsa();
	}
	printf("test_bn: %lu iterations ok\n", iters);
	return 0;
}
