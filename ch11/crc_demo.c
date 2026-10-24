// SPDX-License-Identifier: MIT
/*
 * crc_demo.c - the standard check values, and how the byte table is
 * built from its eight one-bit entries.
 */
#include <stdio.h>
#include "crc.h"

int main(void)
{
	static uint32_t tab[256], t8[8][256];
	const uint8_t *msg = (const uint8_t *)"123456789";

	crc32_table_init(CRC32_POLY_LE, tab);
	crc32_slice8_init(CRC32_POLY_LE, t8);
	printf("CRC-32(\"123456789\")  bitwise %08x  bytewise %08x  "
	       "slice-by-8 %08x\n",
	       ~crc32_bitwise(~0u, msg, 9, CRC32_POLY_LE),
	       ~crc32_bytewise(~0u, msg, 9, tab),
	       ~crc32_slice8(~0u, msg, 9, t8));
	crc32_table_init(CRC32C_POLY_LE, tab);
	printf("CRC-32C(\"123456789\") bitwise %08x  bytewise %08x\n",
	       ~crc32_bitwise(~0u, msg, 9, CRC32C_POLY_LE),
	       ~crc32_bytewise(~0u, msg, 9, tab));

	crc32_table_init(CRC32_POLY_LE, tab);
	printf("CRC-32 table: [0x01] %08x  [0x02] %08x  [0x03] %08x"
	       " = [0x01]^[0x02]\n", tab[1], tab[2], tab[3]);
	printf("table sizes: bytewise %zu bytes, slice-by-8 %zu bytes\n",
	       sizeof(tab), sizeof(t8));
	return 0;
}
