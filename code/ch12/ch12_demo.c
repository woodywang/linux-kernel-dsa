// SPDX-License-Identifier: MIT
/*
 * ch12_demo.c - the numbers quoted in Chapter 12: a binary GCD trace,
 * the convergents behind a best rational approximation, operation
 * counts for schoolbook and Karatsuba multiplication and for three
 * ways to exponentiate, and RSA decryption with and without the CRT.
 * Everything is deterministic; nothing here measures time.
 */
#include <stdio.h>
#include <string.h>
#include "bn.h"
#include "numth.h"
#include "rsa.h"

static unsigned long long rng_state = 2026;

static uint64_t rnd(void)
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return rng_state * 2685821657736338717ULL;
}

static void gcd_demo(unsigned long a, unsigned long b)
{
	unsigned long a0 = a, b0 = b, steps = 0;

	/* The loop of gcd_binary(), printing each step. */
	printf("binary gcd(%lu, %lu): ", a, b);
	b >>= ffs0(b);
	for (;;) {
		a >>= ffs0(a);
		printf("(%lu,%lu) ", a, b);
		if (a == 1 || a == b)
			break;
		if (a < b)
			swap(a, b);
		a -= b;
		steps++;
	}
	printf("-> %lu after %lu subtractions\n", gcd_binary(a0, b0), steps);

	unsigned long x = a0, y = b0, divs = 0;

	printf("Euclid: ");
	while (y) {
		unsigned long t = x % y;

		printf("(%lu,%lu) ", x, y);
		x = y;
		y = t;
		divs++;
	}
	printf("-> %lu after %lu divisions\n", x, divs);
}

static void gcd_averages(unsigned long max)
{
	unsigned long divs = 0, subs = 0, pairs = 0;

	for (unsigned long a = 1; a < max; a++)
		for (unsigned long b = 1; b < max; b++) {
			unsigned long x = a, y = b;

			while (y) {
				unsigned long t = x % y;

				x = y;
				y = t;
				divs++;
			}
			x = a >> ffs0(a);
			y = b >> ffs0(b);
			while (x != y) {
				if (x < y)
					swap(x, y);
				x -= y;
				x >>= ffs0(x);
				subs++;
			}
			pairs++;
		}
	printf("pairs below %lu: Euclid %.2f divisions, binary %.2f subtractions on average\n",
	       max, (double)divs / pairs, (double)subs / pairs);
}

static void rational_demo(unsigned long gn, unsigned long gd)
{
	unsigned long n = gn, d = gd, n0 = 0, d0 = 1, n1 = 1, d1 = 0, bn, bd;

	printf("%lu/%lu: terms and convergents:", gn, gd);
	while (d) {
		unsigned long a = n / d, t = n % d;

		n = d;
		d = t;
		unsigned long n2 = n0 + a * n1, d2 = d0 + a * d1;

		printf(" [%lu] %lu/%lu", a, n2, d2);
		n0 = n1;
		n1 = n2;
		d0 = d1;
		d1 = d2;
	}
	printf("\n");
	static const unsigned long max_d[] = { 31, 60, 100 };

	for (int i = 0; i < 3; i++) {
		best_rational(gn, gd, 1000, max_d[i], &bn, &bd);
		printf("  best with n <= 1000, d <= %3lu: %lu/%lu\n", max_d[i],
		       bn, bd);
	}
}

static void mul_demo(void)
{
	static limb_t a[128], b[128], r[256], tmp[4 * 128 + 64];

	for (int i = 0; i < 128; i++) {
		a[i] = (limb_t)rnd();
		b[i] = (limb_t)rnd();
	}
	printf("limb multiplications (32-bit limbs, threshold %d):\n",
	       KARATSUBA_THRESHOLD);
	for (int n = 16; n <= 128; n *= 2) {
		unsigned long school, kara;

		bn_stats.limb_muls = 0;
		mpn_mul_basecase(r, a, n, b, n);
		school = bn_stats.limb_muls;
		bn_stats.limb_muls = 0;
		mpn_mul_n(r, a, b, n, tmp);
		kara = bn_stats.limb_muls;
		printf("  %4d bits: schoolbook %5lu, Karatsuba %5lu (%.0f%%)\n",
		       n * 32, school, kara, 100.0 * kara / school);
	}
}

static void count(const char *what)
{
	printf("  %-18s %9lu %9lu\n", what,
	       bn_stats.sqr, bn_stats.mul);
	memset(&bn_stats, 0, sizeof(bn_stats));
}

static void powm_demo(const struct rsa_key *k)
{
	struct bn e, x, r;

	bn_set_u64(&x, 0x1234567);
	bn_set_u64(&e, 65537);
	memset(&bn_stats, 0, sizeof(bn_stats));
	printf("exponentiation modulo a %d-bit n:\n%20s %9s %9s\n",
	       bn_bits(&k->n), "", "squarings", "products");
	bn_powm(&r, &x, &e, &k->n);
	count("e = 65537:");
	bn_powm(&r, &x, &k->d, &k->n);
	count("d, binary:");
	for (int w = 4; w <= 5; w++) {
		char label[32];

		bn_powm_window(&r, &x, &k->d, &k->n, w);
		snprintf(label, sizeof(label), "d, window k = %d:", w);
		count(label);
	}
	bn_powm_ladder(&r, &x, &k->d, &k->n, bn_bits(&k->n));
	count("d, ladder:");
}

static void rsa_demo(const struct rsa_key *k)
{
	struct bn m, c, m1, m2;
	unsigned long direct, crt;
	char hex[1200];

	bn_set_u64(&m, 0x4b65726e656cULL);		/* "Kernel" */
	rsa_enc(k, &c, &m);
	bn_stats.limb_muls = 0;
	rsa_dec_direct(k, &m1, &c);
	direct = bn_stats.limb_muls;
	bn_stats.limb_muls = 0;
	rsa_dec_crt(k, &m2, &c);
	crt = bn_stats.limb_muls;
	bn_to_hex(&m2, hex);
	printf("RSA-%d: p %d bits, q %d bits, d %d bits\n", bn_bits(&k->n),
	       bn_bits(&k->p), bn_bits(&k->q), bn_bits(&k->d));
	printf("  decrypted 0x%s; direct and CRT agree: %s\n", hex,
	       bn_cmp(&m1, &m2) ? "no" : "yes");
	printf("  limb multiplications: direct %lu, CRT %lu (%.2fx fewer)\n",
	       direct, crt, (double)direct / crt);
}

int main(void)
{
	static struct rsa_key key;

	gcd_demo(1071, 462);
	gcd_averages(1000);
	printf("\n");
	rational_demo(31415, 10000);
	printf("\n");
	mul_demo();
	printf("\n");
	rsa_keygen(&key, 2048, rnd);
	powm_demo(&key);
	printf("\n");
	rsa_demo(&key);
	return 0;
}
