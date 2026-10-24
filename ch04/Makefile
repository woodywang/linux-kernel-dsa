# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined

all: test_bitmap ida_demo cpumask_demo atomic_demo

test_bitmap: test_bitmap.c bitmap.h idalloc.h cpumask.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

ida_demo: ida_demo.c bitmap.h idalloc.h
	$(CC) $(CFLAGS) -o $@ $<

cpumask_demo: cpumask_demo.c bitmap.h cpumask.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

atomic_demo: atomic_demo.c bitmap.h
	$(CC) $(CFLAGS) -pthread -o $@ $<

# Run the randomized tests and the demos whose output the chapter quotes.
# atomic_demo runs only its atomic half here ("q"); its non-atomic half
# loses a machine-dependent number of updates (Exercise 4.2).
check: test_bitmap ida_demo cpumask_demo atomic_demo
	./test_bitmap 20000 1
	./ida_demo
	./cpumask_demo
	./atomic_demo q

clean:
	rm -f test_bitmap ida_demo cpumask_demo atomic_demo

.PHONY: all check clean
