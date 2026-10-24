// SPDX-License-Identifier: MIT
/*
 * cpumask.h - a teaching version of the kernel's CPU masks.
 *
 * A cpumask is a bitmap with one bit per CPU number.  Its type is sized
 * for NR_CPUS, the compile-time maximum, but only the first nr_cpu_ids
 * bits (the number of CPU IDs this machine can actually have, found at
 * boot) are ever looked at.  Masks that would otherwise live on the stack
 * are allocated at only cpumask_size() bytes, which is why such a mask
 * must never be copied with a structure assignment.
 *
 * What it leaves out on purpose: the CONFIG_CPUMASK_OFFSTACK=n variant
 * (where cpumask_var_t is a one-element array on the stack), the
 * compile-time single-word fast paths, the system masks (possible,
 * present, online, active), and hotplug.
 */
#ifndef CH04_CPUMASK_H
#define CH04_CPUMASK_H

#include <stdlib.h>
#include "bitmap.h"

#define NR_CPUS		8192		/* as with x86-64's largest setting */

static unsigned int nr_cpu_ids = NR_CPUS;	/* set once, at "boot" */

struct cpumask {
	DECLARE_BITMAP(bits, NR_CPUS);
};

#define cpumask_bits(maskp)	((maskp)->bits)

/* Bytes really needed for one mask on this machine. */
static inline unsigned int cpumask_size(void)
{
	return BITS_TO_LONGS(nr_cpu_ids) * sizeof(unsigned long);
}

/* Off-stack masks: a pointer to cpumask_size() bytes, not to a whole struct. */
typedef struct cpumask *cpumask_var_t;

static inline bool zalloc_cpumask_var(cpumask_var_t *mask)
{
	*mask = calloc(1, cpumask_size());
	return *mask != NULL;
}

static inline void free_cpumask_var(cpumask_var_t mask)
{
	free(mask);
}

/* Every operation stops at nr_cpu_ids, never at NR_CPUS. */
#define for_each_cpu(cpu, mask)						\
	for_each_set_bit(cpu, cpumask_bits(mask), nr_cpu_ids)

static inline void cpumask_set_cpu(unsigned int cpu, struct cpumask *dst)
{
	set_bit(cpu, cpumask_bits(dst));
}

static inline void cpumask_clear_cpu(unsigned int cpu, struct cpumask *dst)
{
	clear_bit(cpu, cpumask_bits(dst));
}

static inline bool cpumask_test_cpu(unsigned int cpu, const struct cpumask *m)
{
	return test_bit(cpu, cpumask_bits(m));
}

static inline void cpumask_copy(struct cpumask *dst, const struct cpumask *src)
{
	memcpy(dst, src, cpumask_size());	/* never *dst = *src */
}

static inline bool cpumask_and(struct cpumask *dst, const struct cpumask *a,
			       const struct cpumask *b)
{
	return bitmap_and(cpumask_bits(dst), cpumask_bits(a), cpumask_bits(b),
			  nr_cpu_ids);
}

static inline unsigned int cpumask_weight(const struct cpumask *m)
{
	return bitmap_weight(cpumask_bits(m), nr_cpu_ids);
}

static inline unsigned int cpumask_next(int n, const struct cpumask *m)
{
	return find_next_bit(cpumask_bits(m), nr_cpu_ids, n + 1);
}

static inline unsigned int cpumask_first_and(const struct cpumask *a,
					     const struct cpumask *b)
{
	return find_next_and_bit(cpumask_bits(a), cpumask_bits(b), nr_cpu_ids, 0);
}

#endif /* CH04_CPUMASK_H */
