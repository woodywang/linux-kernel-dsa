// SPDX-License-Identifier: MIT
/*
 * crc_bench.c - time the three CRC-32 methods on one buffer (Exercise
 * 11.3).  Results depend entirely on your machine; none are quoted in
 * the book.
 *
 *   ./crc_bench [buffer-size-in-KiB]
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "crc.h"

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	size_t kib = argc > 1 ? strtoul(argv[1], NULL, 0) : 16384;
	size_t len = kib * 1024;
	static uint32_t tab[256], t8[8][256];
	uint8_t *buf = malloc(len);
	uint32_t c[3];
	double t[4];

	if (!buf)
		return 1;
	for (size_t i = 0; i < len; i++)
		buf[i] = (uint8_t)(i * 2654435761u >> 13);
	crc32_table_init(CRC32_POLY_LE, tab);
	crc32_slice8_init(CRC32_POLY_LE, t8);

	t[0] = now();
	c[0] = crc32_bitwise(~0u, buf, len, CRC32_POLY_LE);
	t[1] = now();
	c[1] = crc32_bytewise(~0u, buf, len, tab);
	t[2] = now();
	c[2] = crc32_slice8(~0u, buf, len, t8);
	t[3] = now();

	printf("%zu KiB, CRCs %s\n", kib,
	       c[0] == c[1] && c[1] == c[2] ? "agree" : "DISAGREE");
	printf("bitwise    %8.1f MB/s\n", len / (t[1] - t[0]) / 1e6);
	printf("bytewise   %8.1f MB/s\n", len / (t[2] - t[1]) / 1e6);
	printf("slice-by-8 %8.1f MB/s\n", len / (t[3] - t[2]) / 1e6);
	free(buf);
	return 0;
}
