.PHONY: all clean test

CC ?= gcc
CFLAGS ?= -O2 -Wall

all: libkcp.a

libkcp.a: ikcp.o
	$(AR) rcs $@ $^

ikcp.o: ikcp.c ikcp.h
	$(CC) $(CFLAGS) -c ikcp.c -o $@

# unit + performance tests for the rcv_buf skip list (issue #331)
test_skx: test_skx.c ikcp.c ikcp.h
	$(CC) $(CFLAGS) test_skx.c -o $@

test: test_skx
	./test_skx

clean:
	rm -f libkcp.a ikcp.o test_skx
