// SPDX-License-Identifier: MIT
/*
 * numth.h - word-sized number theory in the style of the kernel's
 * lib/math/: Euclid's and the binary GCD (two variants, as the kernel
 * has), lcm, the best rational approximation by continued fractions,
 * the shift-and-subtract integer square root, and a growable sieve of
 * Eratosthenes.
 *
 * Left out: the kernel's static key that picks a GCD variant at boot,
 * the 32-bit int_sqrt64(), and the sieve's RCU publication and its
 * trial-division fallback when memory runs out.
 */
#ifndef NUMTH_H
#define NUMTH_H

#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define swap(a, b) do { typeof(a) _t = (a); (a) = (b); (b) = _t; } while (0)

/* Number of trailing zero bits; x must not be 0 (like the kernel's __ffs). */
static inline unsigned int ffs0(unsigned long x)
{
	return (unsigned int)__builtin_ctzl(x);
}

/* ---------------------------------------------------------------- GCD */

/* The common approach: one division per step. */
static inline unsigned long gcd_euclid(unsigned long a, unsigned long b)
{
	while (b) {
		unsigned long r = a % b;

		a = b;
		b = r;
	}
	return a;
}

/* Binary GCD for CPUs that count trailing zeros in one instruction. */
static inline unsigned long gcd_binary(unsigned long a, unsigned long b)
{
	unsigned long r = a | b;	/* lowest set bit: the common 2^k */

	if (!a || !b)
		return r;

	b >>= ffs0(b);			/* make b odd */
	if (b == 1)
		return r & -r;

	for (;;) {
		a >>= ffs0(a);		/* make a odd */
		if (a == 1)
			return r & -r;
		if (a == b)
			return a << ffs0(r);

		if (a < b)
			swap(a, b);
		a -= b;			/* odd - odd = even */
	}
}

/*
 * Binary GCD for CPUs without a fast trailing-zero count.  The common
 * factor 2^k stays in both numbers, and bit r plays the role of bit 0.
 * Each subtraction step divides by 4: it keeps whichever of a - b and
 * a + b is a multiple of 4r.
 */
static inline unsigned long gcd_evenodd(unsigned long a, unsigned long b)
{
	unsigned long r = a | b;

	if (!a || !b)
		return r;

	r &= -r;			/* isolate the lowest set bit */

	while (!(b & r))
		b >>= 1;
	if (b == r)
		return r;

	for (;;) {
		while (!(a & r))
			a >>= 1;
		if (a == r)
			return r;
		if (a == b)
			return a;

		if (a < b)
			swap(a, b);
		a -= b;
		a >>= 1;
		if (a & r)
			a += b;		/* (a - b)/2 + b = (a + b)/2 */
		a >>= 1;
	}
}

/* Divide first, so that the result overflows only if the lcm does. */
static inline unsigned long lcm(unsigned long a, unsigned long b)
{
	if (a && b)
		return (a / gcd_binary(a, b)) * b;
	return 0;
}

/* ---------------------------------------- best rational approximation */

/*
 * Find n/d close to given_n/given_d with n <= max_n and d <= max_d.
 * The continued-fraction terms come from Euclid's algorithm; each one
 * gives the next convergent.  When a convergent exceeds a limit, the
 * answer is the previous convergent or the largest semi-convergent
 * that fits, whichever is closer.
 */
static inline void best_rational(unsigned long given_n, unsigned long given_d,
				 unsigned long max_n, unsigned long max_d,
				 unsigned long *best_n, unsigned long *best_d)
{
	unsigned long n = given_n, d = given_d;
	unsigned long n0 = 0, d0 = 1;	/* convergent k-2 */
	unsigned long n1 = 1, d1 = 0;	/* convergent k-1 */

	while (d) {
		unsigned long dp = d, a = n / d;	/* next term */

		d = n % d;
		n = dp;

		unsigned long n2 = n0 + a * n1, d2 = d0 + a * d1;

		if (n2 > max_n || d2 > max_d) {
			unsigned long t = ULONG_MAX;	/* semi-convergent term */

			if (d1)
				t = (max_d - d0) / d1;
			if (n1 && (max_n - n0) / n1 < t)
				t = (max_n - n0) / n1;
			/* Half rule: is the semi-convergent the closer one? */
			if (!d1 || 2 * t > a || (2 * t == a && d0 * dp > d1 * d)) {
				n1 = n0 + t * n1;
				d1 = d0 + t * d1;
			}
			break;
		}
		n0 = n1;
		n1 = n2;
		d0 = d1;
		d1 = d2;
	}
	*best_n = n1;
	*best_d = d1;
}

/* ------------------------------------------------- integer square root */

/* floor(sqrt(x)), one result bit per iteration, no multiplication. */
static inline unsigned long isqrt(unsigned long x)
{
	unsigned long b, m, y = 0;

	if (x <= 1)
		return x;

	/* Highest power of 4 that is <= x. */
	m = 1UL << ((63 - __builtin_clzl(x)) & ~1UL);
	while (m) {
		b = y + m;
		y >>= 1;
		if (x >= b) {
			x -= b;
			y += m;
		}
		m >>= 2;
	}
	return y;
}

/* ------------------------------------------------------------- sieve */

/* A bitmap of primes below sz; grown on demand, never shrunk. */
struct primes {
	unsigned long sz, last;		/* last: largest prime below sz */
	unsigned char *bits;		/* bits[x] != 0: x is prime */
};

/* Grow the sieve to cover [0, 2x): by Bertrand's postulate it then
 * contains a prime larger than x. */
static inline bool primes_expand(struct primes *p, unsigned long x)
{
	unsigned long sz = 2 * x < 64 ? 64 : 2 * x;
	unsigned char *bits = malloc(sz);

	if (!bits)
		return false;
	memset(bits, 1, sz);
	bits[0] = bits[1] = 0;
	if (p->bits)		/* the old part is already sieved */
		memcpy(bits, p->bits, p->sz);
	for (unsigned long y = 2; y < sz; y++) {
		if (!bits[y])
			continue;
		/* Clear multiples in the new part only. */
		unsigned long m = 2 * y;

		if (m < p->sz)
			m = (p->sz + y - 1) / y * y;
		for (; m < sz; m += y)
			bits[m] = 0;
		p->last = y;
	}
	free(p->bits);
	p->bits = bits;
	p->sz = sz;
	return true;
}

/* The smallest prime larger than x (0 if out of memory). */
static inline unsigned long next_prime(struct primes *p, unsigned long x)
{
	while (x >= p->last)
		if (!primes_expand(p, x))
			return 0;
	while (!p->bits[++x])
		;
	return x;
}

static inline bool is_prime(struct primes *p, unsigned long x)
{
	while (x >= p->sz)
		if (!primes_expand(p, x))
			return false;
	return p->bits[x];
}

#endif /* NUMTH_H */
