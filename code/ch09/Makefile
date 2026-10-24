# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined
HDRS = list.h container_of.h swap.h

TESTS = test_sort test_list_sort test_min_heap test_plist
DEMOS = sort_demo bench

all: $(TESTS) $(DEMOS)

test_sort: test_sort.c sort.c sort.h $(HDRS)
	$(CC) $(CFLAGS) $(SAN) -o $@ test_sort.c sort.c -lm

test_list_sort: test_list_sort.c list_sort.c list_sort.h $(HDRS)
	$(CC) $(CFLAGS) $(SAN) -o $@ test_list_sort.c list_sort.c

test_min_heap: test_min_heap.c min_heap.h $(HDRS)
	$(CC) $(CFLAGS) $(SAN) -o $@ test_min_heap.c

test_plist: test_plist.c plist.c plist.h $(HDRS)
	$(CC) $(CFLAGS) $(SAN) -o $@ test_plist.c plist.c

sort_demo: sort_demo.c sort.c list_sort.c sort.h list_sort.h min_heap.h $(HDRS)
	$(CC) $(CFLAGS) -o $@ sort_demo.c sort.c list_sort.c -lm

# Timing experiments for the exercises; not run by "make check".
bench: bench.c sort.c sort.h $(HDRS)
	$(CC) $(CFLAGS) -o $@ bench.c sort.c

# Run the randomized tests, then the demo whose output the chapter quotes.
check: $(TESTS) sort_demo
	./test_sort 3000 1
	./test_list_sort 2000 1
	./test_min_heap 200000 1
	./test_plist 300000 1
	./sort_demo

clean:
	rm -f $(TESTS) $(DEMOS)

.PHONY: all check clean
