// SPDX-License-Identifier: MIT
/*
 * sim_demo.c - run every policy on four synthetic traces and print the
 * hit rates.  The output is deterministic; Section 17.6 quotes it.
 *
 *   cc -O2 -Wall -o sim_demo sim_demo.c reclaim.c && ./sim_demo
 */
#include "reclaim.h"
#include "traces.h"

#include <stdio.h>
#include <stdlib.h>

#define CAP	1024		/* pages of memory */
#define LEN	200000		/* accesses per trace */

static void pct(long hits, long n)
{
	long permille = (hits * 1000 + n / 2) / n;

	printf(" %7ld.%ld", permille / 10, permille % 10);
}

int main(void)
{
	static const struct {
		const char *name;
		long (*gen)(uint32_t *, long, int);
	} traces[] = {
		{ "loop",  trace_loop },
		{ "scan",  trace_scan },
		{ "skew",  trace_skew },
		{ "shift", trace_shift },
	};
	uint32_t *t = malloc(LEN * sizeof(*t));

	if (!t)
		return 1;
	printf("hit rate (%%), %d pages of memory, %d accesses per trace\n",
	       CAP, LEN);
	printf("%-6s", "trace");
	for (int p = 0; p < NR_POLICIES; p++)
		printf(" %9s", policy_name[p]);
	printf("\n");

	for (unsigned int k = 0; k < sizeof(traces) / sizeof(traces[0]); k++) {
		rng_seed(17 + k);
		long n = traces[k].gen(t, LEN, CAP);

		printf("%-6s", traces[k].name);
		for (int p = 0; p < NR_POLICIES; p++) {
			long h = sim_run(p, CAP, t, n, NULL);

			pct(h, n);
		}
		printf("\n");
	}
	free(t);
	return 0;
}
