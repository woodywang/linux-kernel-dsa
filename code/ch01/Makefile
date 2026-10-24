# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined

all: two_lists false_sharing test_ilist

two_lists: two_lists.c ilist.h container_of.h
	$(CC) $(CFLAGS) -o $@ $<

false_sharing: false_sharing.c
	$(CC) $(CFLAGS) -pthread -o $@ $<

test_ilist: test_ilist.c ilist.h container_of.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

# Run the tests, and make sure the type checks reject bad code.
check: test_ilist two_lists
	./test_ilist 1000000 1
	./two_lists
	@if $(CC) -c -o /dev/null typecheck_fail.c 2>typecheck.log; then \
		echo "FAIL: typecheck_fail.c compiled"; exit 1; \
	else \
		grep -c "pointer type mismatch in container_of()" typecheck.log >/dev/null && \
		grep -c "min() of different types" typecheck.log >/dev/null && \
		echo "ok: both type errors were rejected"; \
	fi
	@rm -f typecheck.log

clean:
	rm -f two_lists false_sharing test_ilist typecheck.log

.PHONY: all check clean
