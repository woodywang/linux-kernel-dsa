# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined
HDRS = mini_slub.h ilist.h container_of.h

all: test_mini_slub slub_demo

# The randomized test includes mini_slub.c to reach its internals.
test_mini_slub: test_mini_slub.c mini_slub.c $(HDRS)
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

# The demo whose output the chapter quotes.
slub_demo: slub_demo.c mini_slub.c $(HDRS)
	$(CC) $(CFLAGS) $(SAN) -o $@ slub_demo.c mini_slub.c

check: all
	./test_mini_slub 300000 1
	./test_mini_slub 300000 2
	./slub_demo

clean:
	rm -f test_mini_slub slub_demo

.PHONY: all check clean
