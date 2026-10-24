# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined

all: mini_maple test_mini_maple

mini_maple: mini_maple.c
	$(CC) $(CFLAGS) -o $@ $<

test_mini_maple: test_mini_maple.c mini_maple.c
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

# Run the randomized tests and the demo.  (maple_alloc_demo.c is built
# inside a kernel tree instead; see its header comment.)
check: test_mini_maple mini_maple
	./test_mini_maple 300000 1
	./mini_maple

clean:
	rm -f mini_maple test_mini_maple

.PHONY: all check clean
