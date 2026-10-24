# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined

all: htab_demo lpm_demo test_htab test_lpm test_bloom

htab_demo: htab_demo.c htab.c htab.h jhash.h
	$(CC) $(CFLAGS) -o $@ htab_demo.c htab.c

lpm_demo: lpm_demo.c lpm.c lpm.h
	$(CC) $(CFLAGS) -o $@ lpm_demo.c lpm.c

test_htab: test_htab.c htab.c htab.h jhash.h
	$(CC) $(CFLAGS) $(SAN) -o $@ test_htab.c htab.c

test_lpm: test_lpm.c lpm.c lpm.h
	$(CC) $(CFLAGS) $(SAN) -o $@ test_lpm.c lpm.c

test_bloom: test_bloom.c bloom.c bloom.h jhash.h
	$(CC) $(CFLAGS) $(SAN) -o $@ test_bloom.c bloom.c -lm

# Randomized tests under ASan and UBSan, then the programs whose output
# the chapter quotes (test_bloom prints its measurements).
check: test_htab test_lpm test_bloom htab_demo lpm_demo
	./test_htab 400000 1
	./test_htab 400000 2
	./test_lpm 200000 1
	./test_lpm 200000 2
	./test_bloom
	./htab_demo
	./lpm_demo

# Optional (Exercise 20.5): compare the teaching maps with real kernel maps
# through the bpf() system call.  Needs Linux 5.16 or later and root (or
# CAP_BPF plus CAP_SYS_ADMIN); not part of 'make check'.
live_maps: live_maps.c htab.c lpm.c bloom.c htab.h lpm.h bloom.h jhash.h
	$(CC) $(CFLAGS) -o $@ live_maps.c htab.c lpm.c bloom.c

live: live_maps
	./live_maps

clean:
	rm -f htab_demo lpm_demo test_htab test_lpm test_bloom live_maps

.PHONY: all check live clean
