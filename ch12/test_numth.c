// SPDX-License-Identifier: MIT
/*
 * test_numth.c - randomized tests for numth.h.
 *
 *   ./test_numth [iterations] [seed]
 *
 * The three GCDs must agree with each other on every pair (all pairs
 * below 300, then random pairs with random common powers of two), lcm()
 * must satisfy gcd * lcm = a * b, best_rational() must return a fraction
 * within the limits that is as close as the best one found by brute
 * force (plus the cases from the kernel's KUnit test), isqrt() must
 * return floor(sqrt(x)), and the sieve must agree with trial division.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "numth.h"

typedef unsigned __int128 u128;

static unsigned long long rng_state;

static unsigned long rnd(void)
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return (unsigned long)(rng_state * 2685821657736338717ULL);
}

/* A random number with a random number of bits. */
static unsigned long rnd_bits(void)
{
	return rnd() >> (rnd() % 64);
}

static void check_gcd(unsigned long a, unsigned long b)
{
	unsigned long g = gcd_euclid(a, b);

	assert(gcd_binary(a, b) == g);
	assert(gcd_evenodd(a, b) == g);
	if (a && b) {
		unsigned long l = lcm(a, b);

		if ((u128)a / g * b <= ULONG_MAX)
			assert((u128)l * g == (u128)a * b);
	} else {
		assert(lcm(a, b) == 0);
	}
}

/* Is |n1/d1 - x| < |n2/d2 - x| (-1), equal (0) or greater (1)? x = gn/gd. */
static int closer(unsigned long n1, unsigned long d1, unsigned long n2,
		  unsigned long d2, unsigned long gn, unsigned long gd)
{
	/* |n/d - gn/gd| = |n gd - gn d| / (d gd); compare e1/d1 with e2/d2. */
	__int128 e1 = (__int128)n1 * gd - (__int128)gn * d1;
	__int128 e2 = (__int128)n2 * gd - (__int128)gn * d2;

	if (e1 < 0)
		e1 = -e1;
	if (e2 < 0)
		e2 = -e2;
	/* e1 * d2 vs e2 * d1: e < 2^84, d < 2^20, so this fits. */
	__int128 l = e1 * (__int128)d2, r = e2 * (__int128)d1;

	return l < r ? -1 : l > r;
}

static void check_rational(unsigned long gn, unsigned long gd,
			   unsigned long mn, unsigned long md)
{
	unsigned long n, d, bn = 0, bd = 1;

	best_rational(gn, gd, mn, md, &n, &d);
	assert(d >= 1 && d <= md && n <= mn);

	/* Brute force: for each d, the nearest allowed numerators. */
	for (unsigned long dd = 1; dd <= md; dd++) {
		unsigned long lo = (unsigned long)((u128)gn * dd / gd);

		for (unsigned long nn = lo ? lo - 1 : 0; nn <= lo + 1; nn++) {
			unsigned long c = nn > mn ? mn : nn;

			if (closer(c, dd, bn, bd, gn, gd) < 0) {
				bn = c;
				bd = dd;
			}
		}
	}
	if (closer(n, d, bn, bd, gn, gd) != 0) {
		printf("rational %lu/%lu, max %lu/%lu: got %lu/%lu, best %lu/%lu\n",
		       gn, gd, mn, md, n, d, bn, bd);
		abort();
	}
}

int main(int argc, char **argv)
{
	unsigned long iters = argc > 1 ? strtoul(argv[1], NULL, 0) : 100000;
	static const unsigned long kunit[][6] = {	/* lib/math/tests/rational_kunit.c */
		{ 1230, 10, 100, 20, 100, 1 },
		{ 34567, 100, 120, 20, 120, 1 },
		{ 1, 30, 100, 10, 0, 1 },
		{ 1, 19, 100, 10, 1, 10 },
		{ 27, 32, 16, 16, 11, 13 },
		{ 1155, 7735, 255, 255, 33, 221 },
		{ 87, 32, 70, 32, 68, 25 },
		{ 14533, 4626, 15000, 2400, 7433, 2366 },
	};
	struct primes sieve = { 0 };

	rng_state = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
	if (!rng_state)
		rng_state = 1;

	/* GCD */
	for (unsigned long a = 0; a < 300; a++)
		for (unsigned long b = 0; b < 300; b++)
			check_gcd(a, b);
	check_gcd(ULONG_MAX, ULONG_MAX);
	check_gcd(ULONG_MAX, 1);
	check_gcd(1UL << 63, 1UL << 62);
	for (unsigned long i = 0; i < iters; i++) {
		unsigned long a = rnd_bits(), b = rnd_bits(), k = rnd() % 20;

		check_gcd(a, b);
		check_gcd(a << k, b << k);
		check_gcd(a | 1, b & ~1UL);
	}

	/* Best rational approximation */
	for (unsigned int i = 0; i < sizeof(kunit) / sizeof(kunit[0]); i++) {
		unsigned long n, d;

		best_rational(kunit[i][0], kunit[i][1], kunit[i][2], kunit[i][3],
			      &n, &d);
		assert(n == kunit[i][4] && d == kunit[i][5]);
		check_rational(kunit[i][0], kunit[i][1], kunit[i][2], kunit[i][3]);
	}
	for (unsigned long i = 0; i < iters / 20; i++) {
		unsigned long gd = 1 + rnd() % 1000000;
		unsigned long gn = rnd() % (1 + (rnd() % 4 ? gd * 4 : 100000000));
		unsigned long mn = 1 + rnd() % 400, md = 1 + rnd() % 400;

		check_rational(gn, gd, mn, md);
	}

	/* Integer square root */
	for (unsigned long i = 0; i < iters; i++) {
		unsigned long x = rnd_bits(), y;

		if (i % 3 == 0) {		/* near a perfect square */
			unsigned long s = rnd() >> 32;

			x = s * s + (rnd() % 3) - 1;
		}
		y = isqrt(x);
		assert((u128)y * y <= x && (u128)(y + 1) * (y + 1) > x);
	}
	assert(isqrt(ULONG_MAX) == 0xffffffffUL);

	/* Sieve: query in a random order, so that it grows irregularly. */
	for (unsigned long i = 0; i < iters / 10; i++) {
		unsigned long x = rnd() % (i < iters / 20 ? 2000 : 50000);
		bool prime = x >= 2;

		for (unsigned long y = 2; y * y <= x; y++)
			if (x % y == 0)
				prime = false;
		assert(is_prime(&sieve, x) == prime);

		unsigned long p = next_prime(&sieve, x);

		assert(p > x && is_prime(&sieve, p));
		for (unsigned long y = x + 1; y < p; y++)
			assert(!is_prime(&sieve, y));
	}
	free(sieve.bits);
	printf("test_numth: %lu iterations ok\n", iters);
	return 0;
}
