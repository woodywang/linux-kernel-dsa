// SPDX-License-Identifier: MIT
/*
 * atomic_demo.c - what "atomic" buys: no lost updates.
 *
 * Two threads set bits in the same words of a shared bitmap.  Thread 0
 * owns bits 0-31 of every word, thread 1 owns bits 32-63, so they never
 * touch the same bit, only the same words.  With set_bit() every bit ends
 * up set.  With __set_bit() a thread can load a word, the other thread
 * can store to it, and the first thread's store then wipes out that
 * change: some bits stay clear.  The number lost depends on the machine
 * and the run (it is 0 on a single CPU); run it a few times.
 *
 *   cc -O2 -Wall -pthread -o atomic_demo atomic_demo.c && ./atomic_demo
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "bitmap.h"

#define WORDS	(1UL << 16)
#define NBITS	(WORDS * BITS_PER_LONG)

static unsigned long *map;
static bool atomic_ops;
static pthread_barrier_t start;

static void *worker(void *arg)
{
	unsigned long first = (unsigned long)arg * 32;

	pthread_barrier_wait(&start);
	for (unsigned long b = first; b < first + 32; b++)
		for (unsigned long w = 0; w < WORDS; w++) {
			if (atomic_ops)
				set_bit(w * BITS_PER_LONG + b, map);
			else
				__set_bit(w * BITS_PER_LONG + b, map);
		}
	return NULL;
}

static unsigned long run(bool atomic)
{
	pthread_t t[2];

	bitmap_zero(map, NBITS);
	atomic_ops = atomic;
	pthread_barrier_init(&start, NULL, 2);
	for (long i = 0; i < 2; i++)
		pthread_create(&t[i], NULL, worker, (void *)i);
	for (int i = 0; i < 2; i++)
		pthread_join(t[i], NULL);
	pthread_barrier_destroy(&start);
	return NBITS - bitmap_weight(map, NBITS);
}

int main(int argc, char **argv)
{
	unsigned long lost;

	map = malloc(WORDS * sizeof(unsigned long));
	if (!map)
		return 1;
	lost = run(true);
	printf("set_bit():   %lu of %lu bits lost\n", lost, NBITS);
	if (lost) {
		printf("FAIL: atomic set_bit() lost updates\n");
		return 1;
	}
	if (argc > 1 && argv[1][0] == 'q')	/* "quick": skip the racy part */
		return 0;
	printf("__set_bit(): %lu of %lu bits lost\n", run(false), NBITS);
	free(map);
	return 0;
}
