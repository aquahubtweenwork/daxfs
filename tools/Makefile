# SPDX-License-Identifier: GPL-2.0

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += -I../include

TARGETS = daxfs-branch daxfs-inspect

all: $(TARGETS)

daxfs-branch: daxfs-branch.c
	$(CC) $(CFLAGS) -o $@ $<

daxfs-inspect: daxfs-inspect.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)

.PHONY: all clean
