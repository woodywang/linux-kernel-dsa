# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined
# Some distributions build with _FORTIFY_SOURCE by default, which hides
# some memory accesses from ThreadSanitizer; turn it off for TSan builds.
TSAN = -fsanitize=thread -U_FORTIFY_SOURCE
URCU = urcu.c urcu.h cblist.h container_of.h

PROGS = test_cblist cblist_demo stress_rcu stress_rcu_tsan stress_rcu_broken \
	typesafe typesafe_tsan typesafe_norecheck

all: $(PROGS)

test_cblist: test_cblist.c cblist.h container_of.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

cblist_demo: cblist_demo.c cblist.h container_of.h
	$(CC) $(CFLAGS) -o $@ $<

stress_rcu: stress_rcu.c rculist.h $(URCU)
	$(CC) $(CFLAGS) $(SAN) -pthread -o $@ stress_rcu.c urcu.c

# ThreadSanitizer cannot be combined with AddressSanitizer: separate builds.
stress_rcu_tsan: stress_rcu.c rculist.h $(URCU)
	$(CC) $(CFLAGS) $(TSAN) -pthread -o $@ stress_rcu.c urcu.c

# Grace periods that don't wait for readers (Exercise 13.2).
stress_rcu_broken: stress_rcu.c rculist.h $(URCU)
	$(CC) $(CFLAGS) $(SAN) -DURCU_BROKEN -pthread -o $@ stress_rcu.c urcu.c

typesafe: typesafe.c rculist.h $(URCU)
	$(CC) $(CFLAGS) $(SAN) -pthread -o $@ typesafe.c urcu.c

typesafe_tsan: typesafe.c rculist.h $(URCU)
	$(CC) $(CFLAGS) $(TSAN) -pthread -o $@ typesafe.c urcu.c

# The lookup without its second look at the key (Exercise 13.4).
typesafe_norecheck: typesafe.c rculist.h $(URCU)
	$(CC) $(CFLAGS) $(SAN) -DNO_RECHECK -pthread -o $@ typesafe.c urcu.c

# Run the model test and the demo the chapter quotes, the stress tests
# under ASan/UBSan and under TSan, and make sure the two broken variants
# are caught.
check: all
	./test_cblist 300000 1
	./cblist_demo
	./stress_rcu 300000 1
	./stress_rcu_tsan 100000 2
	./typesafe 2000000 1
	./typesafe_tsan 300000 2
	@if ./stress_rcu_broken 300000 1 >broken.log 2>&1; then \
		echo "FAIL: no use-after-free without grace periods"; exit 1; \
	elif grep -q "heap-use-after-free" broken.log; then \
		echo "ok: ASan reports a use-after-free when grace periods don't wait"; \
	else \
		cat broken.log; exit 1; \
	fi
	@if ./typesafe_norecheck 2000000 1 >norecheck.log 2>&1; then \
		echo "FAIL: no wrong object without the key recheck"; exit 1; \
	elif grep -q "got the wrong object" norecheck.log; then \
		echo "ok: readers get the wrong object without the key recheck"; \
	else \
		cat norecheck.log; exit 1; \
	fi
	@rm -f broken.log norecheck.log

clean:
	rm -f $(PROGS) broken.log norecheck.log

.PHONY: all check clean
