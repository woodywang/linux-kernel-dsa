# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined

LIB = eevdf.c cfs.c rbtree.c
HDR = eevdf.h cfs.h rbtree.h container_of.h

all: sim test_eevdf

sim: sim.c $(LIB) $(HDR)
	$(CC) $(CFLAGS) -o $@ sim.c $(LIB)

test_eevdf: test_eevdf.c $(LIB) $(HDR)
	$(CC) $(CFLAGS) $(SAN) -o $@ test_eevdf.c $(LIB)

# Run the randomized tests (two seeds), then the simulator the chapter quotes.
check: test_eevdf sim
	./test_eevdf 200000 1
	./test_eevdf 200000 2
	./sim

clean:
	rm -f sim test_eevdf

.PHONY: all check clean
