# Plain make. OpenWrt passes CC/CFLAGS on the command line, so extra flags go
# through `override` to survive that.
CC ?= cc
CFLAGS ?= -O2
override CFLAGS += -std=gnu99 -Wall -Wextra
TESTDIR ?= ../tests

LIB_OBJS = wire.o filter.o
RELAY_OBJS =

all: mabur-relay

mabur-relay: main.o $(RELAY_OBJS) $(LIB_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

unit_tests: $(TESTDIR)/unit_main.c $(TESTDIR)/test_wire.c $(TESTDIR)/test_filter.c $(LIB_OBJS)
	$(CC) $(CFLAGS) -I. -I$(TESTDIR) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o mabur-relay unit_tests

.PHONY: all clean
