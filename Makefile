# SPDX-License-Identifier: GPL-2.0

KDIR ?= /lib/modules/$(shell uname -r)/build

all: daxfs tools

daxfs:
	$(MAKE) -C daxfs KDIR=$(KDIR)

tools:
	$(MAKE) -C tools

tests:
	$(MAKE) -C tests

clean:
	$(MAKE) -C daxfs clean
	$(MAKE) -C tools clean
	$(MAKE) -C tests clean

.PHONY: all daxfs tools tests clean
