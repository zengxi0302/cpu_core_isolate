# SPDX-License-Identifier: GPL-2.0
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra

all: cfimon ownpage

cfimon: cfimon.c ../../kernel/include/uapi/cfi.h
	$(CC) $(CFLAGS) -o $@ cfimon.c

ownpage: ownpage.c
	$(CC) $(CFLAGS) -o $@ ownpage.c

clean:
	rm -f cfimon ownpage

.PHONY: all clean
