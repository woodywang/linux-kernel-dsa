# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined

LIB = rbtree.c itree.c ostree.c
HDR = rbtree.h itree.h ostree.h container_of.h

all: rb_demo test_rbtree

rb_demo: rb_demo.c $(LIB) $(HDR)
	$(CC) $(CFLAGS) -o $@ rb_demo.c $(LIB) -lm

test_rbtree: test_rbtree.c $(LIB) $(HDR)
	$(CC) $(CFLAGS) $(SAN) -o $@ test_rbtree.c $(LIB)

# Run the randomized tests (two seeds), then the demo the chapter quotes.
check: test_rbtree rb_demo
	./test_rbtree 200000 1
	./test_rbtree 200000 2
	./rb_demo

clean:
	rm -f rb_demo test_rbtree

.PHONY: all check clean
