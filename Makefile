#=====================================================================
#
# Makefile - build and test kcp
#
#   make          build the kcp library
#   make test     build and run the automated test suite
#   make demo     build the interactive latency demo (test.cpp)
#   make clean    remove build artifacts
#
#=====================================================================

CC      ?= cc
CXX     ?= c++
CFLAGS  ?= -O2 -Wall
CXXFLAGS ?= -O2 -Wall

LIB      = libkcp.a
LIB_OBJS = ikcp.o
TEST_BIN = tests/stream_test

.PHONY: all test demo clean

all: $(LIB)

$(LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

ikcp.o: ikcp.c ikcp.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TEST_BIN): tests/stream_test.cpp ikcp.c ikcp.h
	$(CXX) $(CXXFLAGS) -I. tests/stream_test.cpp ikcp.c -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

demo: test
	$(CXX) $(CXXFLAGS) test.cpp -o test -lstdc++

clean:
	rm -f $(LIB) $(LIB_OBJS) $(TEST_BIN) test
