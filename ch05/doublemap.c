// SPDX-License-Identifier: MIT
/*
 * doublemap.c - map a ring's data pages twice, back to back, so that a
 * record that wraps around the end is still contiguous in memory.
 *
 *   cc -O2 -Wall -o doublemap doublemap.c && ./doublemap
 *
 * The BPF ring buffer does this in the kernel with vmap() and again when
 * user space mmap()s it (kernel/bpf/ringbuf.c).  Here the "pages" are a
 * memfd: reserve twice the address space, then map the same file into
 * both halves.  A write that runs past the end of the first mapping lands
 * in the second, which is the same memory as the start of the first.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Map size bytes of fd twice, contiguously; returns the base or NULL. */
static unsigned char *map_twice(int fd, size_t size)
{
	unsigned char *base = mmap(NULL, 2 * size, PROT_NONE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (base == MAP_FAILED)
		return NULL;
	if (mmap(base, size, PROT_READ | PROT_WRITE,
		 MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED ||
	    mmap(base + size, size, PROT_READ | PROT_WRITE,
		 MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
		munmap(base, 2 * size);
		return NULL;
	}
	return base;
}

int main(void)
{
	size_t size = (size_t)sysconf(_SC_PAGESIZE);
	const char rec[] = "hello, ring!";
	size_t len = sizeof(rec) - 1, off = size - 6;
	unsigned char *data;
	int fd = memfd_create("ring", 0);

	if (fd < 0 || ftruncate(fd, (off_t)size) < 0 ||
	    !(data = map_twice(fd, size))) {
		perror("doublemap");
		return 1;
	}
	printf("data area: one page, mapped twice back to back\n");

	/* One memcpy, although the record wraps around the end. */
	memcpy(data + off, rec, len);
	printf("wrote %zu bytes at offset size-6: \"%.*s\"\n", len, (int)len,
	       (const char *)data + off);
	printf("first 6 bytes of the page:   \"%.6s\"\n", (const char *)data);
	printf("last 6 bytes of the page:    \"%.6s\"\n",
	       (const char *)data + size - 6);

	munmap(data, 2 * size);
	close(fd);
	return 0;
}
