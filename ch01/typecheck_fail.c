// SPDX-License-Identifier: MIT
/*
 * typecheck_fail.c - this file must NOT compile.  `make check` builds it
 * and expects both mistakes below to be rejected.
 */
#include "ilist.h"

struct task {
	int pid;
	struct list_head run;
};

int main(void)
{
	struct task t = { .pid = 1 };
	int *p = &t.pid;

	/* Mistake 1: p points at an int, but we claim it points at `run`. */
	struct task *back = container_of(p, struct task, run);

	/* Mistake 2: comparing an int with an unsigned long. */
	return min(back->pid, sizeof(t));
}
