# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined

all: test_textsearch ts_demo test_crc crc_demo crc_bench test_raid6 raid6_demo

test_textsearch: test_textsearch.c textsearch.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

ts_demo: ts_demo.c textsearch.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

test_crc: test_crc.c crc.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

crc_demo: crc_demo.c crc.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

crc_bench: crc_bench.c crc.h
	$(CC) $(CFLAGS) -o $@ $<

test_raid6: test_raid6.c raid6.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

raid6_demo: raid6_demo.c raid6.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

# Run the randomized tests and the demos whose output the chapter quotes.
# crc_bench prints machine-dependent timings (Exercise 11.3) and is only
# built here.
check: test_textsearch ts_demo test_crc crc_demo test_raid6 raid6_demo crc_bench
	./test_textsearch 100000 1
	./ts_demo
	./test_crc 200000 1
	./crc_demo
	./test_raid6 20000 1
	./raid6_demo

clean:
	rm -f test_textsearch ts_demo test_crc crc_demo crc_bench test_raid6 raid6_demo

.PHONY: all check clean
