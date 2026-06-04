/*
 * madvise + mlock behaviour check. mlock must prefault+pin; MADV_DONTNEED must
 * drop the pages so an anonymous re-read returns zero (a no-op stub would still
 * see the old pattern); MADV_WILLNEED must keep data intact and return 0.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
	size_t len = 64 * 4096;
	unsigned char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
	                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) { puts("FAIL mmap"); return 1; }

	if (mlock(p, len) != 0) { puts("FAIL mlock"); return 1; }

	memset(p, 0xAB, len);
	for (size_t i = 0; i < len; i += 4096)
		if (p[i] != 0xAB) { puts("FAIL write"); return 1; }

	if (madvise(p, len, MADV_DONTNEED) != 0) { puts("FAIL madvise"); return 1; }
	for (size_t i = 0; i < len; i += 4096)
		if (p[i] != 0) { puts("CORRUPT dontneed-not-zero"); return 1; }

	memset(p, 0xCD, len);
	if (madvise(p, len, MADV_WILLNEED) != 0) { puts("FAIL willneed"); return 1; }
	if (p[0] != 0xCD || p[len - 1] != 0xCD) { puts("FAIL willneed-data"); return 1; }

	munlock(p, len);
	munmap(p, len);
	puts("OK");
	return 0;
}
