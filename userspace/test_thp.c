/* Transparent hugepages via madvise(MADV_HUGEPAGE): an anonymous region mapped
 * with 4K pages is collapsed into 2 MB blocks. Confirms AnonHugePages rises,
 * the data survives the collapse, and a forked child's COW (whole 2 MB copied,
 * then split to 4K) leaves the parent's copy intact. */
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE 14
#endif

#define LEN (8UL << 20)
#define STEP 4096

static long anon_huge_kb(void)
{
	int fd = open("/proc/meminfo", O_RDONLY);
	if (fd < 0) return -1;
	static char buf[4096];
	int n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) return -1;
	buf[n] = 0;
	char *p = strstr(buf, "AnonHugePages:");
	if (!p) return -1;
	p += 14;
	while (*p == ' ') p++;
	long v = 0;
	while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
	return v;
}

int main(void)
{
	char *p = mmap(NULL, LEN, PROT_READ | PROT_WRITE,
	               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) { puts("FAIL mmap"); return 0; }

	for (unsigned long i = 0; i < LEN; i += STEP) p[i] = (char)(i / STEP);

	long before = anon_huge_kb();
	if (madvise(p, LEN, MADV_HUGEPAGE) != 0) { puts("FAIL madvise"); return 0; }
	long after = anon_huge_kb();

	if (before < 0 || after < 0) { puts("FAIL meminfo"); return 0; }
	if (after < before + 2048) { printf("FAIL no-collapse %ld->%ld\n", before, after); return 0; }

	for (unsigned long i = 0; i < LEN; i += STEP)
		if (p[i] != (char)(i / STEP)) { puts("FAIL readback"); return 0; }

	pid_t pid = fork();
	if (pid == 0) {
		for (unsigned long i = 0; i < LEN; i += STEP) p[i] = (char)(i / STEP + 7);  /* COW */
		for (unsigned long i = 0; i < LEN; i += STEP)
			if (p[i] != (char)(i / STEP + 7)) _exit(2);
		_exit(0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { puts("FAIL child"); return 0; }

	for (unsigned long i = 0; i < LEN; i += STEP)
		if (p[i] != (char)(i / STEP)) { puts("FAIL parent-cow"); return 0; }

	munmap(p, LEN);
	puts("OK");
	return 0;
}
