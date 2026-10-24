// SPDX-License-Identifier: MIT
/*
 * cpumask_demo.c - NR_CPUS versus nr_cpu_ids, and off-stack masks.
 *
 *   cc -O2 -Wall -o cpumask_demo cpumask_demo.c && ./cpumask_demo
 */
#include <stdio.h>
#include "cpumask.h"

static struct cpumask possible;		/* a full-size static mask */

static void print_mask(const char *name, const struct cpumask *m)
{
	unsigned int cpu;

	printf("%-8s", name);
	for_each_cpu(cpu, m)
		printf(" %u", cpu);
	printf("  (%u CPUs)\n", cpumask_weight(m));
}

int main(void)
{
	cpumask_var_t online, allowed, both;

	/* "Firmware" reports CPUs 0-11; nr_cpu_ids is the last one plus 1. */
	bitmap_set(cpumask_bits(&possible), 0, 12);
	nr_cpu_ids = find_last_bit(cpumask_bits(&possible), NR_CPUS) + 1;

	printf("NR_CPUS %d: struct cpumask is %zu bytes\n", NR_CPUS,
	       sizeof(struct cpumask));
	printf("nr_cpu_ids %u: an off-stack mask is %u bytes\n", nr_cpu_ids,
	       cpumask_size());

	if (!zalloc_cpumask_var(&online) || !zalloc_cpumask_var(&allowed) ||
	    !zalloc_cpumask_var(&both))
		return 1;

	cpumask_copy(online, &possible);
	cpumask_clear_cpu(4, online);		/* CPU 4 goes offline */
	cpumask_set_cpu(1, allowed);		/* a task's affinity */
	cpumask_set_cpu(4, allowed);
	cpumask_set_cpu(5, allowed);
	cpumask_set_cpu(9, allowed);

	print_mask("online", online);
	print_mask("allowed", allowed);
	cpumask_and(both, online, allowed);
	print_mask("both", both);
	printf("first allowed online CPU: %u; next after it: %u\n",
	       cpumask_first_and(online, allowed), cpumask_next(1, both));

	free_cpumask_var(online);
	free_cpumask_var(allowed);
	free_cpumask_var(both);
	return 0;
}
