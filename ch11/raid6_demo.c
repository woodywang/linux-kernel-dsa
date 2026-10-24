// SPDX-License-Identifier: MIT
/*
 * raid6_demo.c - one byte from each of four data disks, its P and Q,
 * and the recovery of disks 1 and 3.
 */
#include <stdio.h>
#include "raid6.h"

int main(void)
{
	uint8_t d[6] = { 0x37, 0xa5, 0x10, 0xc4 };	/* D0..D3, P, Q */
	uint8_t *ptrs[6];

	gf_init();
	for (int z = 0; z < 6; z++)
		ptrs[z] = &d[z];
	raid6_gen_syndrome(6, 1, ptrs);
	printf("D0..D3 = %02x %02x %02x %02x   P = %02x   Q = %02x\n",
	       d[0], d[1], d[2], d[3], d[4], d[5]);
	printf("g^0..g^3 = %02x %02x %02x %02x\n",
	       gf_pow2(0), gf_pow2(1), gf_pow2(2), gf_pow2(3));

	d[1] = d[3] = 0;			/* lose disks 1 and 3 */
	raid6_recover(6, 1, 1, 3, ptrs);
	printf("recovered D1 = %02x, D3 = %02x\n", d[1], d[3]);
	printf("coefficients: 1/(g^2+1) = %02x, 1/(g^1+g^3) = %02x\n",
	       gf_inv(gf_pow2(2) ^ 1), gf_inv(gf_pow2(1) ^ gf_pow2(3)));
	return 0;
}
