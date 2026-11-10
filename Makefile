# simple Makefile for kcp (mirrors the travis-ci build)
# - `make` / `make all`: build the static library and the test binary
# - `make test`: build and run the test suite (piped stdin: skips the
#   interactive "press enter" prompts)

CC ?= gcc
CXX ?= g++
CFLAGS ?= -O3 -Wall
CXXFLAGS ?= -O3 -Wall

all: libkcp.a test

libkcp.a: ikcp.c ikcp.h
	$(CC) $(CFLAGS) -c ikcp.c -o ikcp.o
	ar rcs libkcp.a ikcp.o

test: test.cpp test.h ikcp.c ikcp.h
	$(CXX) $(CXXFLAGS) test.cpp -o test -lstdc++

# run the full test suite; feeding /dev/null makes the interactive
# test() prompts return immediately instead of blocking
check test-kcp: test
	./test < /dev/null

clean:
	rm -f libkcp.a ikcp.o test

.PHONY: all clean check test-kcp
