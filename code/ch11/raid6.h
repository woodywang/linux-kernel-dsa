// SPDX-License-Identifier: MIT
/*
 * raid6.h - arithmetic in GF(2^8) and RAID-6 P/Q parity with recovery
 * from any two lost blocks, following H. Peter Anvin's "The mathematics
 * of RAID-6" and the layout of the kernel's RAID-6 library:
 *
 *   ptrs[0 .. disks-3]  data blocks D0 .. Dn-1
 *   ptrs[disks-2]       P = D0 + D1 + ... + Dn-1           (XOR)
 *   ptrs[disks-1]       Q = D0 + g*D1 + ... + g^(n-1)*Dn-1  (g = {02})
 *
 * The field is GF(2^8) with the polynomial x^8 + x^4 + x^3 + x^2 + 1
 * (0x11d).  Addition is XOR.
 *
 * Left out: the kernel's SIMD versions and its boot-time benchmark that
 * picks one, the partial-stripe "xor_syndrome" update, and the
 * asynchronous (DMA engine) interface.
 */
#ifndef RAID6_H
#define RAID6_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------ GF(2^8) */

/* Multiply by g = {02}: shift left, reduce by 0x11d if x^8 appears. */
static inline uint8_t gf_mul2(uint8_t a)
{
	return (uint8_t)(a << 1) ^ ((a & 0x80) ? 0x1d : 0);
}

/* Carry-less "shift and add" multiplication, reduced as it goes. */
static inline uint8_t gf_mul(uint8_t a, uint8_t b)
{
	uint8_t r = 0;

	while (b) {
		if (b & 1)
			r ^= a;
		a = gf_mul2(a);
		b >>= 1;
	}
	return r;
}

/* Logarithm tables: every nonzero element is a power of g. */
static uint8_t gf_exp[510], gf_log[256];

static inline void gf_init(void)
{
	uint8_t v = 1;

	for (int i = 0; i < 255; i++) {
		gf_exp[i] = gf_exp[i + 255] = v;	/* doubled: no "mod 255" */
		gf_log[v] = (uint8_t)i;
		v = gf_mul2(v);
	}
}

static inline uint8_t gf_pow2(int e)		/* g^e for any integer e */
{
	e %= 255;
	return gf_exp[e < 0 ? e + 255 : e];
}

static inline uint8_t gf_inv(uint8_t a)		/* a must be nonzero */
{
	return gf_exp[255 - gf_log[a]];
}

/* Eight multiplications by {02} at once, one per byte of a word. */
static inline uint64_t gf_mul2_x8(uint64_t v)
{
	uint64_t top = v & 0x8080808080808080ull;
	uint64_t mask = (top << 1) - (top >> 7);  /* 0xff where top bit set */

	return ((v << 1) & 0xfefefefefefefefeull) ^
	       (mask & 0x1d1d1d1d1d1d1d1dull);
}

/* ------------------------------------------------------------- RAID-6 */

#define RAID6_MAX_BYTES	4096
static const uint8_t raid6_zero[RAID6_MAX_BYTES];

/* P and Q for one stripe; Q by Horner's rule from the highest disk down. */
static inline void raid6_gen_syndrome(int disks, size_t bytes, uint8_t **ptrs)
{
	uint8_t *p = ptrs[disks - 2], *q = ptrs[disks - 1];
	int z0 = disks - 3;			/* highest data disk */

	for (size_t i = 0; i < bytes; i++) {
		uint8_t wp = ptrs[z0][i], wq = wp;

		for (int z = z0 - 1; z >= 0; z--) {
			wp ^= ptrs[z][i];
			wq = gf_mul2(wq) ^ ptrs[z][i];
		}
		p[i] = wp;
		q[i] = wq;
	}
}

/* The same, eight bytes per step; bytes must be a multiple of 8. */
static inline void raid6_gen_syndrome_x8(int disks, size_t bytes,
					 uint8_t **ptrs)
{
	int z0 = disks - 3;

	for (size_t i = 0; i < bytes; i += 8) {
		uint64_t wd, wp, wq;

		memcpy(&wp, &ptrs[z0][i], 8);
		wq = wp;
		for (int z = z0 - 1; z >= 0; z--) {
			memcpy(&wd, &ptrs[z][i], 8);
			wp ^= wd;
			wq = gf_mul2_x8(wq) ^ wd;
		}
		memcpy(&ptrs[disks - 2][i], &wp, 8);
		memcpy(&ptrs[disks - 1][i], &wq, 8);
	}
}

/* Multiplying by a constant c is a lookup in a 256-byte row. */
static inline void gf_mul_row(uint8_t c, uint8_t row[256])
{
	for (int b = 0; b < 256; b++)
		row[b] = gf_mul(c, (uint8_t)b);
}

/* Data blocks x < y are lost; P and Q are intact. */
static inline void raid6_2data_recov(int disks, size_t bytes, int x, int y,
				     uint8_t **ptrs)
{
	uint8_t *p = ptrs[disks - 2], *q = ptrs[disks - 1];
	uint8_t *dx = ptrs[x], *dy = ptrs[y];
	uint8_t pmul[256], qmul[256];

	/* Syndrome of the survivors (lost blocks read as zero), computed
	 * into the lost blocks' buffers: dx = Pxy, dy = Qxy. */
	ptrs[x] = ptrs[y] = (uint8_t *)raid6_zero;
	ptrs[disks - 2] = dx;
	ptrs[disks - 1] = dy;
	raid6_gen_syndrome(disks, bytes, ptrs);
	ptrs[x] = dx;
	ptrs[y] = dy;
	ptrs[disks - 2] = p;
	ptrs[disks - 1] = q;

	/* Dx + Dy = P + Pxy  and  g^x Dx + g^y Dy = Q + Qxy, so
	 * Dy = (P+Pxy) / (g^(y-x) + 1)  +  (Q+Qxy) / (g^x + g^y). */
	gf_mul_row(gf_inv(gf_pow2(y - x) ^ 1), pmul);
	gf_mul_row(gf_inv(gf_pow2(x) ^ gf_pow2(y)), qmul);

	for (size_t i = 0; i < bytes; i++) {
		uint8_t px = p[i] ^ dx[i];	/* = Dx + Dy */
		uint8_t qx = q[i] ^ dy[i];	/* = g^x Dx + g^y Dy */

		dy[i] = pmul[px] ^ qmul[qx];
		dx[i] = dy[i] ^ px;
	}
}

/* Data block x and P are lost; Q is intact. */
static inline void raid6_datap_recov(int disks, size_t bytes, int x,
				     uint8_t **ptrs)
{
	uint8_t *q = ptrs[disks - 1], *dx = ptrs[x];
	uint8_t qmul[256];

	/* P becomes Px (the XOR of the survivors); dx becomes Qx. */
	ptrs[x] = (uint8_t *)raid6_zero;
	ptrs[disks - 1] = dx;
	raid6_gen_syndrome(disks, bytes, ptrs);
	ptrs[x] = dx;
	ptrs[disks - 1] = q;

	gf_mul_row(gf_inv(gf_pow2(x)), qmul);	/* Dx = (Q + Qx) / g^x */
	for (size_t i = 0; i < bytes; i++) {
		dx[i] = qmul[q[i] ^ dx[i]];
		ptrs[disks - 2][i] ^= dx[i];	/* P = Px + Dx */
	}
}

/* Any two lost blocks a != b, data or parity. */
static inline void raid6_recover(int disks, size_t bytes, int a, int b,
				 uint8_t **ptrs)
{
	int P = disks - 2, Q = disks - 1;

	if (a > b) {
		int t = a;

		a = b;
		b = t;
	}
	if (b == Q && a == P) {			/* both parities: recompute */
		raid6_gen_syndrome(disks, bytes, ptrs);
	} else if (b == Q) {			/* data + Q: RAID-5, then Q */
		memcpy(ptrs[a], ptrs[P], bytes);
		for (int z = 0; z < P; z++)
			if (z != a)
				for (size_t i = 0; i < bytes; i++)
					ptrs[a][i] ^= ptrs[z][i];
		raid6_gen_syndrome(disks, bytes, ptrs);
	} else if (b == P) {
		raid6_datap_recov(disks, bytes, a, ptrs);
	} else {
		raid6_2data_recov(disks, bytes, a, b, ptrs);
	}
}

#endif /* RAID6_H */
