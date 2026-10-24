# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined
# A second build with small orders, so that blocks reach MAX_PAGE_ORDER
# and cross pageblock boundaries often in a small zone.
SMALL = -DMAX_PAGE_ORDER=5 -DPAGEBLOCK_ORDER=3

HDRS = buddy.h list.h container_of.h

all: test_buddy test_buddy_small buddy_demo

test_buddy: test_buddy.c buddy.c $(HDRS)
	$(CC) $(CFLAGS) $(SAN) -o $@ test_buddy.c buddy.c

test_buddy_small: test_buddy.c buddy.c $(HDRS)
	$(CC) $(CFLAGS) $(SAN) $(SMALL) -o $@ test_buddy.c buddy.c

buddy_demo: buddy_demo.c buddy.c $(HDRS)
	$(CC) $(CFLAGS) $(SAN) -o $@ buddy_demo.c buddy.c

# Randomized tests against the model, then the demo quoted in the chapter.
check: all
	./test_buddy 200000 1
	./test_buddy_small 200000 2
	./buddy_demo

clean:
	rm -f test_buddy test_buddy_small buddy_demo

.PHONY: all check clean
