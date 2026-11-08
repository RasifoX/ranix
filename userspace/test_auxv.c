/* /proc/self/auxv: the vector the image started with, as getauxval sees it.
 * gdb reads it to place a PIE and to find ld.so; without it breakpoints in
 * a PIE land at unrelocated addresses and no shared library is seen. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/wait.h>

static int check(const char *who)
{
	uint64_t v[128];
	int fd = open("/proc/self/auxv", O_RDONLY);
	if (fd < 0) { printf("FAIL %s open\n", who); return 1; }
	ssize_t n = read(fd, v, sizeof v);
	close(fd);
	if (n < 32 || n % 16) { printf("FAIL %s size %zd\n", who, n); return 1; }
	int ent = 0, ph = 0, pg = 0, end = 0;
	for (int i = 0; i + 1 < n / 8; i += 2) {
		if (v[i] == AT_NULL) { end = 1; break; }
		if (v[i] == AT_ENTRY) ent = v[i + 1] == getauxval(AT_ENTRY);
		if (v[i] == AT_PHDR) ph = v[i + 1] == getauxval(AT_PHDR);
		if (v[i] == AT_PAGESZ) pg = v[i + 1] == 4096;
	}
	if (!(ent && ph && pg && end)) { printf("FAIL %s entry=%d phdr=%d pagesz=%d end=%d\n", who, ent, ph, pg, end); return 1; }
	return 0;
}

int main(void)
{
	if (check("self")) return 0;
	pid_t c = fork();
	if (c == 0) _exit(check("child"));
	int st;
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) return 0;
	printf("OK\n");
	return 0;
}
