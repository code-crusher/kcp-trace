# Makefile for kcp: build library, run tests.
#
# targets:
#   all     - build static library and test binary
#   test    - build and run the full test suite
#   clean   - remove build artifacts

CC      ?= gcc
CXX     ?= g++
CFLAGS  ?= -O2 -g
CXXFLAGS?= -O2 -g

LIB     = libkcp.a
TESTBIN = kcp_test

.PHONY: all test clean

all: $(LIB) $(TESTBIN)

$(LIB): ikcp.c ikcp.h
	$(CC) $(CFLAGS) -c ikcp.c -o ikcp.o
	ar rcs $@ ikcp.o

$(TESTBIN): test.cpp test.h ikcp.c ikcp.h
	$(CXX) $(CXXFLAGS) -I. test.cpp -o $@

test: $(TESTBIN)
	./$(TESTBIN) < /dev/null

clean:
	rm -f $(LIB) $(TESTBIN) ikcp.o
