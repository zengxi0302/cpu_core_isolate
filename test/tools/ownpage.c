// SPDX-License-Identifier: GPL-2.0
/*
 * ownpage - allocate and pin one page, print its PFN, stay alive
 *
 * Companion for MFI debugfs injection tests: injecting against a page
 * this process owns keeps the blast radius confined to this process.
 *
 *   $ sudo ./ownpage &
 *   pfn=0x1a2b3c
 *   $ echo "domain=mem pfn=0x1a2b3c type=ce" > /sys/kernel/debug/cfi/inject
 *
 * Needs root (or CAP_SYS_ADMIN) to read PFNs from /proc/self/pagemap.
 *
 * Build: gcc -O2 -Wall -o ownpage ownpage.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#define PAGEMAP_PFN_MASK	((1ULL << 55) - 1)
#define PAGEMAP_PRESENT		(1ULL << 63)

int main(void)
{
	long page_size = sysconf(_SC_PAGESIZE);
	unsigned char *p;
	uint64_t entry;
	off_t off;
	int fd;

	p = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memset(p, 0xa5, page_size);	/* fault it in */

	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0) {
		perror("open pagemap");
		return 1;
	}
	off = ((uintptr_t)p / page_size) * sizeof(uint64_t);
	if (pread(fd, &entry, sizeof(entry), off) != sizeof(entry)) {
		perror("pread pagemap");
		return 1;
	}
	close(fd);

	if (!(entry & PAGEMAP_PRESENT) || !(entry & PAGEMAP_PFN_MASK)) {
		fprintf(stderr,
			"page not present or PFN hidden (need root)\n");
		return 1;
	}

	printf("pfn=0x%llx\n",
	       (unsigned long long)(entry & PAGEMAP_PFN_MASK));
	fflush(stdout);

	/* Stay alive so the page keeps its owner; SIGBUS = test worked */
	pause();
	return 0;
}
