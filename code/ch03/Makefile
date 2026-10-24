# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined

HDRS = container_of.h hlist.h hash.h hashtable.h siphash.h

all: flood mini_rht test_hashtable test_mini_rht

flood: flood.c $(HDRS)
	$(CC) $(CFLAGS) -o $@ $<

mini_rht: mini_rht.c siphash.h
	$(CC) $(CFLAGS) -o $@ $<

test_hashtable: test_hashtable.c $(HDRS)
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

test_mini_rht: test_mini_rht.c mini_rht.c siphash.h container_of.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

# Run the randomized tests, then the two demos quoted in the chapter.
check: test_hashtable test_mini_rht flood mini_rht
	./test_hashtable 300000 1
	./test_mini_rht 2000000 1
	./flood
	./mini_rht

clean:
	rm -f flood mini_rht test_hashtable test_mini_rht

.PHONY: all check clean
