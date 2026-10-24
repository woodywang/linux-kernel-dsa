// SPDX-License-Identifier: MIT
/*
 * ts_demo.c - search a text that arrives in three blocks.  The match
 * straddles two block boundaries: KMP finds it, Boyer-Moore-Horspool
 * (which looks at each block separately) does not.
 */
#include <stdio.h>
#include "textsearch.h"

int main(void)
{
	static const char *const blocks[] = {
		"We dance the funky ch", "ick", "en tonight",
	};
	struct chunk ch[3];
	struct chunks src = { ch, 3 };
	const char *pat = "chicken";

	for (int i = 0; i < 3; i++) {
		ch[i] = (struct chunk){ (const uint8_t *)blocks[i],
					(unsigned int)strlen(blocks[i]) };
		printf("block %d: \"%s\"\n", i, blocks[i]);
	}

	struct kmp *kmp = kmp_prepare(pat, (unsigned int)strlen(pat));
	struct bmh *bm = bmh_prepare(pat, (unsigned int)strlen(pat));
	struct ts_state s1 = { 0 }, s2 = { 0 };
	unsigned int k = kmp_find(kmp, &s1, chunks_next, &src);
	unsigned int b = bmh_find(bm, &s2, chunks_next, &src);

	printf("prefix table for \"%s\":", pat);
	for (unsigned int i = 0; i < kmp->len; i++)
		printf(" %u", kmp->prefix[i]);
	printf("\n");
	if (k == TS_NOMATCH)
		printf("kmp: no match\n");
	else
		printf("kmp: \"%s\" at offset %u\n", pat, k);
	if (b == TS_NOMATCH)
		printf("bm:  no match\n");
	else
		printf("bm:  \"%s\" at offset %u\n", pat, b);
	free(kmp);
	free(bm);
	return 0;
}
