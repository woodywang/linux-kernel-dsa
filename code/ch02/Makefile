# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined
HDRS = list.h list_nulls.h list_bl.h container_of.h

DEMOS = list_demo proctree nulls_race lru
TESTS = test_list test_bl test_lru

all: $(DEMOS) $(TESTS)

# The demo is built with the corruption checks compiled in.
list_demo: list_demo.c $(HDRS)
	$(CC) $(CFLAGS) -DLIST_HARDENED -o $@ $<

proctree nulls_race lru: %: %.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $<

test_list: test_list.c $(HDRS)
	$(CC) $(CFLAGS) $(SAN) -DLIST_HARDENED -o $@ $<

test_bl: test_bl.c $(HDRS)
	$(CC) $(CFLAGS) $(SAN) -pthread -o $@ $<

test_lru: test_lru.c lru.c $(HDRS)
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

# Run the randomized tests, then the demos whose output the chapter quotes.
check: all
	./test_list 200000 1
	./test_bl 200000 1
	./test_lru 200000 1
	./list_demo
	./proctree
	./nulls_race
	./lru

clean:
	rm -f $(DEMOS) $(TESTS)

.PHONY: all check clean
