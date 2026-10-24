# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined

all: mini_xa test_mini_xa

mini_xa: mini_xa.c
	$(CC) $(CFLAGS) -o $@ $<

test_mini_xa: test_mini_xa.c mini_xa.c
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

# Run the randomized tests with several seeds, then the demo.
check: test_mini_xa mini_xa
	./test_mini_xa 300000 1
	./test_mini_xa 100000 2
	./test_mini_xa 100000 3
	./mini_xa

clean:
	rm -f mini_xa test_mini_xa

.PHONY: all check clean
