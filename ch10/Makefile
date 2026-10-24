# SPDX-License-Identifier: MIT
CFLAGS ?= -O2 -Wall -Wextra
SAN = -fsanitize=address,undefined
SRCS = cfg.c lockgraph.c union_find.c
HDRS = cfg.h lockgraph.h union_find.h ilist.h container_of.h

all: graph_demo test_graph stack_depth

graph_demo: graph_demo.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ graph_demo.c $(SRCS)

test_graph: test_graph.c $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(SAN) -o $@ test_graph.c $(SRCS)

# Exercise 10.1: recursion on a kernel-sized stack (not run by "check").
stack_depth: stack_depth.c cfg.c cfg.h
	$(CC) $(CFLAGS) -pthread -o $@ stack_depth.c cfg.c

# Run the randomized tests and the demo whose output the chapter quotes.
check: test_graph graph_demo
	./test_graph 20000 1
	./graph_demo

clean:
	rm -f graph_demo test_graph stack_depth

.PHONY: all check clean
