# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined

BN = bn.c rsa.c bn.h rsa.h numth.h

all: test_numth test_bn ch12_demo

test_numth: test_numth.c numth.h
	$(CC) $(CFLAGS) $(SAN) -o $@ $<

test_bn: test_bn.c $(BN)
	$(CC) $(CFLAGS) $(SAN) -o $@ test_bn.c bn.c rsa.c

ch12_demo: ch12_demo.c $(BN)
	$(CC) $(CFLAGS) $(SAN) -o $@ ch12_demo.c bn.c rsa.c

# Run the randomized tests and the demo whose output the chapter quotes.
check: test_numth test_bn ch12_demo
	./test_numth 200000 1
	./test_bn 20000 1
	./ch12_demo

clean:
	rm -f test_numth test_bn ch12_demo

.PHONY: all check clean
