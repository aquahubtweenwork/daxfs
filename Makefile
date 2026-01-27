# SPDX-License-Identifier: GPL-2.0

KDIR ?= /lib/modules/$(shell uname -r)/build

all: daxfs mkdaxfs tools

daxfs:
	$(MAKE) -C daxfs KDIR=$(KDIR)

mkdaxfs:
	$(MAKE) -C mkdaxfs

tools:
	$(MAKE) -C tools

clean:
	$(MAKE) -C daxfs clean
	$(MAKE) -C mkdaxfs clean
	$(MAKE) -C tools clean

.PHONY: all daxfs mkdaxfs tools clean
