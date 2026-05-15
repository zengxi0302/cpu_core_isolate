# SPDX-License-Identifier: GPL-2.0
#
# Top-level Makefile for building the CFI kernel module out-of-tree.
#
# Usage:
#   make                    # Build against running kernel
#   make KDIR=/path/to/src  # Build against specific kernel source
#   make clean              # Clean build artifacts
#

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

.PHONY: default clean install
