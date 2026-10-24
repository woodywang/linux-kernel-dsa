// SPDX-License-Identifier: MIT
/*
 * rsa.c - RSA with the Chinese Remainder Theorem; see rsa.h.
 */
#include <assert.h>
#include "numth.h"
#include "rsa.h"

/* Like the kernel, accept only 1 < x < n - 1. */
static int check_payload(const struct bn *x, const struct bn *n)
{
	struct bn one, n1;

	bn_set_u64(&one, 1);
	bn_sub(&n1, n, &one);
	if (bn_cmp(x, &one) <= 0 || bn_cmp(x, &n1) >= 0)
		return -1;
	return 0;
}

/* c = m^e mod n */
int rsa_enc(const struct rsa_key *k, struct bn *c, const struct bn *m)
{
	if (check_payload(m, &k->n))
		return -1;
	bn_powm(c, m, &k->e, &k->n);
	return 0;
}

/* m = c^d mod n, the slow way. */
int rsa_dec_direct(const struct rsa_key *k, struct bn *m, const struct bn *c)
{
	if (check_payload(c, &k->n))
		return -1;
	bn_powm(m, c, &k->d, &k->n);
	return 0;
}

/*
 * m = c^d mod n with two half-size exponentiations:
 *   m1 = c^dP mod p,  m2 = c^dQ mod q,
 *   h  = (m1 - m2) qInv mod p,
 *   m  = m2 + q h.
 */
int rsa_dec_crt(const struct rsa_key *k, struct bn *m, const struct bn *c)
{
	struct bn m1, m2, t, h;

	if (check_payload(c, &k->n))
		return -1;
	bn_powm(&m1, c, &k->dp, &k->p);
	bn_powm(&m2, c, &k->dq, &k->q);

	bn_mod(&t, &m2, &k->p);			/* h = (m1 - m2) mod p */
	if (bn_cmp(&m1, &t) >= 0) {
		bn_sub(&h, &m1, &t);
	} else {
		bn_add(&h, &m1, &k->p);
		bn_sub(&h, &h, &t);
	}
	bn_mulm(&h, &h, &k->qinv, &k->p);	/* h = h qInv mod p */

	bn_mul(&t, &k->q, &h);			/* m = m2 + q h < p q */
	bn_add(m, &m2, &t);
	return 0;
}

/* ------------------------------------------------ toy key generation */

static void bn_random(struct bn *r, int bits, uint64_t (*rnd)(void))
{
	r->n = (bits + LIMB_BITS - 1) / LIMB_BITS;
	for (int i = 0; i < r->n; i++)
		r->d[i] = (limb_t)rnd();
	if (bits % LIMB_BITS)
		r->d[r->n - 1] &= ((limb_t)1 << (bits % LIMB_BITS)) - 1;
	/* Top two bits set, so that p q has exactly 2 * bits bits; odd. */
	r->d[(bits - 1) / LIMB_BITS] |= (limb_t)1 << ((bits - 1) % LIMB_BITS);
	r->d[(bits - 2) / LIMB_BITS] |= (limb_t)1 << ((bits - 2) % LIMB_BITS);
	r->d[0] |= 1;
	bn_norm(r);
}

/* Trial division by small primes, then Fermat tests to bases 2, 3, 5, 7. */
static bool probably_prime(const struct bn *x)
{
	static struct primes sieve;
	struct bn a, x1, one, r;

	for (unsigned long p = 2; p < 2000; p = next_prime(&sieve, p))
		if (bn_mod_1(x, (limb_t)p) == 0)
			return false;
	bn_set_u64(&one, 1);
	bn_sub(&x1, x, &one);
	for (uint64_t base = 2; base <= 7; base += base == 2 ? 1 : 2) {
		bn_set_u64(&a, base);
		bn_powm(&r, &a, &x1, x);
		if (bn_cmp(&r, &one))
			return false;
	}
	return true;
}

/* A prime p of the given size with gcd(e, p - 1) = 1, e = 65537 prime. */
static void random_prime(struct bn *p, int bits, uint64_t (*rnd)(void))
{
	do {
		bn_random(p, bits, rnd);
	} while (bn_mod_1(p, 65537) == 1 || !probably_prime(p));
}

void rsa_keygen(struct rsa_key *k, int bits, uint64_t (*rnd)(void))
{
	struct bn one, two, p1, q1, phi, t;

	bn_set_u64(&one, 1);
	bn_set_u64(&two, 2);
	bn_set_u64(&k->e, 65537);
	random_prime(&k->p, bits / 2, rnd);
	do {
		random_prime(&k->q, bits - bits / 2, rnd);
	} while (!bn_cmp(&k->p, &k->q));
	bn_mul(&k->n, &k->p, &k->q);

	/* d = e^-1 mod phi: find j < e with e | j phi + 1. */
	bn_sub(&p1, &k->p, &one);
	bn_sub(&q1, &k->q, &one);
	bn_mul(&phi, &p1, &q1);
	uint64_t f = bn_mod_1(&phi, 65537), j = 1;

	while ((j * f + 1) % 65537)
		j++;
	bn_set_u64(&t, j);
	bn_mul(&t, &t, &phi);
	bn_add(&t, &t, &one);
	bn_divrem(&k->d, NULL, &t, &k->e);

	bn_mod(&k->dp, &k->d, &p1);
	bn_mod(&k->dq, &k->d, &q1);
	/* qInv = q^(p-2) mod p, by Fermat's little theorem. */
	bn_sub(&t, &k->p, &two);
	bn_powm(&k->qinv, &k->q, &t, &k->p);
}
