# SPDX-License-Identifier: GPL-2.0

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += -I../include

TARGETS = mkdaxfs

all: $(TARGETS)

mkdaxfs: mkdaxfs.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGETS)

install: $(TARGETS)
	install -m 755 $(TARGETS) /usr/local/bin/

.PHONY: all clean install
