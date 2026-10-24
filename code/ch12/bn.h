// SPDX-License-Identifier: MIT
/*
 * bn.h - a small big-integer library in the style of the kernel's MPI
 * code (lib/crypto/mpi/), which came from GnuPG and GMP.
 *
 * Numbers are arrays of 32-bit "limbs", least significant first.  The
 * mpn_*() functions work on raw limb arrays of given lengths, like the
 * kernel's mpihelp_*(); the bn_*() functions work on struct bn, a
 * non-negative number with a fixed capacity.
 *
 * Multiplication is schoolbook below KARATSUBA_THRESHOLD limbs and
 * Karatsuba above; reduction is long division (Knuth's Algorithm D);
 * modular exponentiation is left-to-right square-and-multiply, as in
 * the kernel's mpi_powm().  A sliding-window version and a Montgomery
 * ladder are included for comparison; the kernel has neither.
 *
 * Left out: the kernel's 64-bit limbs, signs, dynamic allocation (struct
 * bn has a fixed capacity, enough for 2048-bit moduli), assembler inner
 * loops, Karatsuba squaring, and the skipping of limbs equal to 0 or 1
 * in the kernel's schoolbook loops.
 */
#ifndef BN_H
#define BN_H

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t limb_t;
typedef uint64_t dlimb_t;
#define LIMB_BITS 32
#define BN_MAX 136		/* limbs: a 2048-bit square plus slack */

struct bn {
	int n;			/* limbs in use; d[n - 1] != 0, or n == 0 */
	limb_t d[BN_MAX];
};

/* Operation counters, for the demos and exercises. */
struct bn_stats {
	unsigned long limb_muls;	/* 32x32-bit multiplications */
	unsigned long sqr, mul;		/* modular squarings and products */
};
extern struct bn_stats bn_stats;
extern int karatsuba_threshold;		/* default KARATSUBA_THRESHOLD */
#define KARATSUBA_THRESHOLD 16

/* Raw limb arrays. */
limb_t mpn_add_n(limb_t *r, const limb_t *a, const limb_t *b, int n);
limb_t mpn_sub_n(limb_t *r, const limb_t *a, const limb_t *b, int n);
limb_t mpn_add_1(limb_t *r, const limb_t *a, int n, limb_t v);
limb_t mpn_addmul_1(limb_t *r, const limb_t *a, int n, limb_t v);
int mpn_cmp(const limb_t *a, const limb_t *b, int n);
void mpn_mul_basecase(limb_t *r, const limb_t *a, int an,
		      const limb_t *b, int bn);
void mpn_mul_n(limb_t *r, const limb_t *a, const limb_t *b, int n,
	       limb_t *tmp);
void mpn_mul(limb_t *r, const limb_t *a, int an, const limb_t *b, int bn);
void mpn_divrem(limb_t *q, limb_t *r, const limb_t *u, int un,
		const limb_t *v, int vn);

/* struct bn. Results may alias arguments. */
void bn_set_u64(struct bn *r, uint64_t v);
uint64_t bn_get_u64(const struct bn *a);
void bn_norm(struct bn *a);
int bn_cmp(const struct bn *a, const struct bn *b);
int bn_bits(const struct bn *a);
bool bn_bit(const struct bn *a, int i);
void bn_add(struct bn *r, const struct bn *a, const struct bn *b);
void bn_sub(struct bn *r, const struct bn *a, const struct bn *b);
void bn_mul(struct bn *r, const struct bn *a, const struct bn *b);
void bn_divrem(struct bn *q, struct bn *r, const struct bn *a,
	       const struct bn *m);
limb_t bn_mod_1(const struct bn *a, limb_t m);
void bn_mod(struct bn *r, const struct bn *a, const struct bn *m);
void bn_mulm(struct bn *r, const struct bn *a, const struct bn *b,
	     const struct bn *m);
void bn_sqrm(struct bn *r, const struct bn *a, const struct bn *m);
void bn_powm(struct bn *r, const struct bn *base, const struct bn *e,
	     const struct bn *m);
void bn_powm_window(struct bn *r, const struct bn *base, const struct bn *e,
		    const struct bn *m, int k);
void bn_powm_ladder(struct bn *r, const struct bn *base, const struct bn *e,
		    const struct bn *m, int nbits);
void bn_cswap(struct bn *a, struct bn *b, limb_t bit);
void bn_to_hex(const struct bn *a, char *buf);

#endif /* BN_H */
