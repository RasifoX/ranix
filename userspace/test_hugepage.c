/* MAP_HUGETLB: a 4 MB anonymous region backed by two 2 MB blocks. Writes a
 * pattern across both, forks, and has the child rewrite its copy — exercising
 * the copy-on-write split of a huge block — then confirms the parent's copy is
 * untouched. The returned address must be huge-page aligned. */
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>

#define HUGE (2UL * 1024 * 1024)

int main(void)
{
	size_t len = 2 * HUGE;
	char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
	               MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	if (p == MAP_FAILED) { puts("FAIL mmap"); return 0; }
	if ((unsigned long)p & (HUGE - 1)) { puts("FAIL align"); return 0; }

	for (size_t i = 0; i < len; i += 4096) p[i] = (char)(i / 4096);
	for (size_t i = 0; i < len; i += 4096)
		if (p[i] != (char)(i / 4096)) { puts("FAIL readback"); return 0; }

	pid_t pid = fork();
	if (pid == 0) {
		for (size_t i = 0; i < len; i += 4096) p[i] = (char)(i / 4096 + 1);   /* COW */
		for (size_t i = 0; i < len; i += 4096)
			if (p[i] != (char)(i / 4096 + 1)) _exit(2);
		_exit(0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { puts("FAIL child"); return 0; }

	for (size_t i = 0; i < len; i += 4096)
		if (p[i] != (char)(i / 4096)) { puts("FAIL parent-cow"); return 0; }

	if (munmap(p, len) != 0) { puts("FAIL munmap"); return 0; }
	puts("OK");
	return 0;
}
