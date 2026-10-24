# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined

all: sim_demo test_reclaim test_bloom

sim_demo: sim_demo.c reclaim.c reclaim.h traces.h ilist.h container_of.h
	$(CC) $(CFLAGS) -o $@ sim_demo.c reclaim.c

test_reclaim: test_reclaim.c reclaim.c reclaim.h traces.h ilist.h container_of.h
	$(CC) $(CFLAGS) $(SAN) -o $@ test_reclaim.c reclaim.c

test_bloom: test_bloom.c bloom.h
	$(CC) $(CFLAGS) $(SAN) -o $@ test_bloom.c -lm

# Model checks under ASan and UBSan, then the two programs whose output
# the chapter quotes (test_bloom prints its measurements).
check: test_reclaim test_bloom sim_demo
	./test_reclaim 400 1
	./test_reclaim 400 2
	./test_bloom
	./sim_demo

clean:
	rm -f sim_demo test_reclaim test_bloom

.PHONY: all check clean
