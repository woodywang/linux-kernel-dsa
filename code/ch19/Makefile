# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined
HDRS = wheel.h heapq.h bitmap.h hlist.h container_of.h

TESTS = test_wheel test_wheel_hz100 test_heapq
DEMOS = wheel_demo bench

all: $(TESTS) $(DEMOS)

test_wheel: test_wheel.c wheel.c $(HDRS)
	$(CC) $(CFLAGS) $(SAN) -o $@ test_wheel.c wheel.c

# The same test for a kernel built with HZ=100 (one level fewer).
test_wheel_hz100: test_wheel.c wheel.c $(HDRS)
	$(CC) $(CFLAGS) $(SAN) -DHZ=100 -o $@ test_wheel.c wheel.c

test_heapq: test_heapq.c $(HDRS)
	$(CC) $(CFLAGS) $(SAN) -o $@ test_heapq.c

wheel_demo: wheel_demo.c wheel.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ wheel_demo.c wheel.c

# Timing experiment for Exercise 19.3; not run by "make check".
bench: bench.c wheel.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ bench.c wheel.c

# Exercise 19.4: the wheel without its mark on expiring timers.
test_wheel_nomark: test_wheel.c wheel.c $(HDRS)
	$(CC) $(CFLAGS) -DNO_EXPIRING_MARK -o $@ test_wheel.c wheel.c

# Run the randomized tests, then the demo whose output the chapter quotes.
check: $(TESTS) wheel_demo
	./test_wheel 300000 1
	./test_wheel_hz100 300000 2
	./test_heapq 300000 1
	./wheel_demo

clean:
	rm -f $(TESTS) $(DEMOS) test_wheel_nomark

.PHONY: all check clean
