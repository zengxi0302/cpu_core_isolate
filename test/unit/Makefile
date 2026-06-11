# SPDX-License-Identifier: GPL-2.0
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra

all: test_mfi_policy

test_mfi_policy: test_mfi_policy.c ../../kernel/core/mfi_policy.h
	$(CC) $(CFLAGS) -o $@ test_mfi_policy.c

run: all
	./test_mfi_policy

clean:
	rm -f test_mfi_policy

.PHONY: all run clean
