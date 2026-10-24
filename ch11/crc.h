// SPDX-License-Identifier: MIT
/*
 * crc.h - CRC-32 three ways: one bit at a time, one byte at a time with a
 * 256-entry table (the only generic method the kernel keeps since 6.14),
 * and eight bytes at a time with eight tables ("slice-by-8", which the
 * kernel used by default before 6.14).
 *
 * The functions follow the kernel's crc32_le() convention: bits are
 * processed least significant first ("reflected"), the polynomial is
 * given bit-reversed, and the caller chooses the initial value and any
 * final inversion.  The standard CRC-32 (Ethernet, zlib) and CRC-32C
 * (iSCSI, ext4, Btrfs) both start from ~0 and invert the result.
 *
 * Left out: big-endian (msb-first) CRCs, CRCs of other widths, and the
 * carry-less-multiplication "folding" used by CPU-specific code.
 */
#ifndef CRC_H
#define CRC_H

#include <stddef.h>
#include <stdint.h>

#define CRC32_POLY_LE	0xedb88320u	/* x^32 + x^26 + ... + 1, reversed */
#define CRC32C_POLY_LE	0x82f63b78u	/* Castagnoli, reversed */

/* Long division over GF(2), one message bit per step. */
static inline uint32_t crc32_bitwise(uint32_t crc, const uint8_t *p,
				     size_t len, uint32_t poly)
{
	while (len--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc >> 1) ^ ((crc & 1) ? poly : 0);
	}
	return crc;
}

/*
 * tab[b] is the effect of byte b on the register after 8 steps.  The
 * map is linear over GF(2), so only the eight one-bit bytes need real
 * work; every other entry is an XOR of two entries already computed.
 */
static inline void crc32_table_init(uint32_t poly, uint32_t tab[256])
{
	tab[0] = 0;
	for (int bit = 1; bit < 256; bit <<= 1) {
		uint8_t b = (uint8_t)bit;

		tab[bit] = crc32_bitwise(0, &b, 1, poly);
	}
	for (int i = 1; i < 256; i++) {
		int low = i & -i;		/* lowest set bit of i */

		tab[i] = tab[low] ^ tab[i ^ low];
	}
}

/* One table lookup per byte (Sarwate's method). */
static inline uint32_t crc32_bytewise(uint32_t crc, const uint8_t *p,
				      size_t len, const uint32_t tab[256])
{
	while (len--)
		crc = (crc >> 8) ^ tab[(crc ^ *p++) & 0xff];
	return crc;
}

/* t[k][b]: byte b followed by k zero bytes. */
static inline void crc32_slice8_init(uint32_t poly, uint32_t t[8][256])
{
	crc32_table_init(poly, t[0]);
	for (int k = 1; k < 8; k++)
		for (int b = 0; b < 256; b++)
			t[k][b] = (t[k - 1][b] >> 8) ^ t[0][t[k - 1][b] & 0xff];
}

static inline uint32_t load_le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Eight independent lookups per eight bytes. */
static inline uint32_t crc32_slice8(uint32_t crc, const uint8_t *p,
				    size_t len, const uint32_t t[8][256])
{
	for (; len >= 8; p += 8, len -= 8) {
		uint32_t lo = crc ^ load_le32(p), hi = load_le32(p + 4);

		crc = t[7][lo & 0xff] ^ t[6][(lo >> 8) & 0xff] ^
		      t[5][(lo >> 16) & 0xff] ^ t[4][lo >> 24] ^
		      t[3][hi & 0xff] ^ t[2][(hi >> 8) & 0xff] ^
		      t[1][(hi >> 16) & 0xff] ^ t[0][hi >> 24];
	}
	return crc32_bytewise(crc, p, len, t[0]);
}

#endif /* CRC_H */
