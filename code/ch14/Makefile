# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined
# Some distributions build with _FORTIFY_SOURCE by default, which turns
# memcpy() into __memcpy_chk(), and ThreadSanitizer does not see accesses
# made by the latter; turn it off so that every access is checked.
TSAN = -fsanitize=thread -U_FORTIFY_SOURCE

PROGS = test_pcpu_counter test_llist test_seq stress_counter stress_llist \
	stress_seq stress_seq_noretry aba_demo latch_demo bench_counter

all: $(PROGS)

# Single-threaded tests against simple models, under ASan and UBSan.
test_pcpu_counter: test_pcpu_counter.c pcpu_counter.h
	$(CC) $(CFLAGS) $(SAN) -pthread -o $@ $<

test_llist: test_llist.c llist.h container_of.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

test_seq: test_seq.c latch.h seqcount.h
	$(CC) $(CFLAGS) $(SAN) -pthread -o $@ $<

# Multi-threaded stress tests under ThreadSanitizer (it cannot be
# combined with AddressSanitizer, hence separate builds).
stress_counter: stress_counter.c pcpu_counter.h
	$(CC) $(CFLAGS) $(TSAN) -pthread -o $@ $<

stress_llist: stress_llist.c llist.h container_of.h
	$(CC) $(CFLAGS) $(TSAN) -pthread -o $@ $<

stress_seq: stress_seq.c latch.h seqcount.h
	$(CC) $(CFLAGS) $(TSAN) -pthread -o $@ $<

# The same stress test with the readers' retry check removed
# (Exercise 14.4); not run by "make check".
stress_seq_noretry: stress_seq.c latch.h seqcount.h
	$(CC) $(CFLAGS) -DSEQ_NO_RETRY -pthread -o $@ $<

# Demos whose output the chapter quotes.
aba_demo: aba_demo.c llist.h container_of.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

latch_demo: latch_demo.c latch.h seqcount.h
	$(CC) $(CFLAGS) $(SAN) -pthread -o $@ $<

# Timing exercise only; not run by "make check".
bench_counter: bench_counter.c pcpu_counter.h
	$(CC) $(CFLAGS) -pthread -o $@ $<

check: all
	./test_pcpu_counter 100000 1
	./test_llist 300000 1
	./test_seq 100000 1
	./stress_counter 100000 1
	./stress_llist 100000 1
	./stress_seq 100000 1
	./aba_demo
	./latch_demo

clean:
	rm -f $(PROGS)

.PHONY: all check clean
