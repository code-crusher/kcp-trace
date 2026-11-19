# Makefile for kcp: build the library and run the test suite.
#
# targets:
#   all     - build the static library and the test binaries
#   test    - build and run the full test suite
#   clean   - remove build artifacts

CC       ?= gcc
CXX      ?= g++
CFLAGS   ?= -O2 -g
CXXFLAGS ?= -O2 -g

LIB      = libkcp.a
TESTBIN  = kcp_test
FASTACK  = kcp_test_fastack

.PHONY: all test clean

all: $(LIB) $(TESTBIN) $(FASTACK)

$(LIB): ikcp.c ikcp.h
	$(CC) $(CFLAGS) -c ikcp.c -o ikcp.o
	ar rcs $@ ikcp.o

$(TESTBIN): test.cpp test.h ikcp.c ikcp.h
	$(CXX) $(CXXFLAGS) -I. test.cpp -o $@

$(FASTACK): test_fastack.cpp ikcp.c ikcp.h
	$(CXX) $(CXXFLAGS) -I. test_fastack.cpp -o $@

# stdin is redirected so the interactive prompts never block in CI
test: $(TESTBIN) $(FASTACK)
	./$(TESTBIN) < /dev/null
	./$(FASTACK)

clean:
	rm -f $(LIB) $(TESTBIN) $(FASTACK) ikcp.o
