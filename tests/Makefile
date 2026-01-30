# SPDX-License-Identifier: GPL-2.0

CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2 -g

TESTS = test_mmap

all: $(TESTS)

test_mmap: test_mmap.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TESTS)

# Run tests (requires DAXFS mounted at /mnt/daxfs or specify MOUNTPOINT)
MOUNTPOINT ?= /mnt/daxfs

test: $(TESTS)
	@echo "Running tests on $(MOUNTPOINT)..."
	@for t in $(TESTS); do ./$$t $(MOUNTPOINT) || exit 1; done

.PHONY: all clean test
