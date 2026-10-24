# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined
# Some distributions build with _FORTIFY_SOURCE by default, which turns
# memcpy() into __memcpy_chk(), and ThreadSanitizer does not see accesses
# made by the latter; turn it off so that every copy is checked.
TSAN = -fsanitize=thread -U_FORTIFY_SOURCE

all: test_spsc stress_spsc stress_spsc_relaxed ring_demo doublemap

test_spsc: test_spsc.c spsc.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

# ThreadSanitizer cannot be combined with AddressSanitizer: separate builds.
stress_spsc: stress_spsc.c spsc.h
	$(CC) $(CFLAGS) $(TSAN) -pthread -o $@ $<

# The same test with the acquire/release orderings removed (Exercise 5.1).
stress_spsc_relaxed: stress_spsc.c spsc.h
	$(CC) $(CFLAGS) $(TSAN) -DSPSC_RELAXED -pthread -o $@ $<

ring_demo: ring_demo.c spsc.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

doublemap: doublemap.c
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

# Run the model test, the threaded stress test, the demos the chapter
# quotes, and make sure ThreadSanitizer catches the relaxed version.
check: all
	./test_spsc 50000 1
	./stress_spsc 2000000 1
	./ring_demo
	./doublemap
	@if TSAN_OPTIONS=halt_on_error=1 ./stress_spsc_relaxed 300000 1 \
			>relaxed.log 2>&1; then \
		echo "FAIL: no race reported without acquire/release"; exit 1; \
	elif grep -q "ThreadSanitizer: data race" relaxed.log; then \
		echo "ok: ThreadSanitizer reports a data race without acquire/release"; \
	else \
		cat relaxed.log; exit 1; \
	fi
	@rm -f relaxed.log

clean:
	rm -f test_spsc stress_spsc stress_spsc_relaxed ring_demo doublemap relaxed.log

.PHONY: all check clean
