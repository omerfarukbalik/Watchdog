CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -pedantic -g -D_XOPEN_SOURCE=700 -Iinclude
OBJS    = src/main.o src/signal_manager.o src/process_manager.o src/logger.o
HEADERS = $(wildcard include/*.h)

all: watchdog worker

watchdog: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

worker: workers/worker.c
	$(CC) $(CFLAGS) -o $@ $<

# Rebuild objects when any shared header changes.
src/%.o: src/%.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

test: all
	bash tests/test_scenarios.sh

clean:
	rm -f $(OBJS) watchdog worker watchdog.log

.PHONY: all test clean
