/*
 * hugetlbfs file-backed huge-page mapping. Reserve a few 2 MB pages, mount the
 * fs, create a file, size it, mmap it, and write/read through the mapping —
 * which is backed by the 2 MB pool, not ordinary anon pages.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>

#define HSZ (2 * 1024 * 1024)

int main(void)
{
	int sf = open("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages", O_WRONLY);
	if (sf >= 0) { write(sf, "8\n", 2); close(sf); }   /* reserve; non-fatal if on-demand */

	mkdir("/huge", 0755);
	if (mount("none", "/huge", "hugetlbfs", 0, NULL) != 0) { puts("FAIL mount"); return 1; }

	int fd = open("/huge/buf", O_RDWR | O_CREAT, 0644);
	if (fd < 0) { puts("FAIL open"); return 1; }
	if (ftruncate(fd, HSZ) != 0) { puts("FAIL ftruncate"); return 1; }

	unsigned char *p = mmap(NULL, HSZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) { puts("FAIL mmap"); return 1; }

	memset(p, 0x5A, HSZ);
	for (int i = 0; i < HSZ; i += 4096)
		if (p[i] != 0x5A) { puts("FAIL verify"); return 1; }

	munmap(p, HSZ);
	close(fd);
	puts("OK");
	return 0;
}
