/* 1 GB hugepage (MAP_HUGETLB | MAP_HUGE_1GB). Skips where no 1 GB pool is
 * reserved (small-RAM boots). Always exercises map / write / read / unmap of a
 * 1 GB block; when a second block is available it also forks and has the child
 * rewrite its copy (whole-block COW), confirming the parent's copy is intact. */
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
#ifndef MAP_HUGE_1GB
#define MAP_HUGE_1GB (30 << 26)
#endif

#define G    (1UL << 30)
#define STEP (64UL << 20)   /* touch every 64 MB */
#define FLAGS (MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_1GB)

int main(void)
{
	char *p = mmap(NULL, G, PROT_READ | PROT_WRITE, FLAGS, -1, 0);
	if (p == MAP_FAILED) { puts("OK"); return 0; }   /* no 1G pool → skip */

	if ((unsigned long)p & (G - 1)) { puts("FAIL align"); return 0; }
	for (unsigned long i = 0; i < G; i += STEP) p[i] = (char)(i / STEP);
	for (unsigned long i = 0; i < G; i += STEP)
		if (p[i] != (char)(i / STEP)) { puts("FAIL readback"); return 0; }

	/* COW test needs a second block for the copy — only run it if one exists. */
	char *q = mmap(NULL, G, PROT_READ | PROT_WRITE, FLAGS, -1, 0);
	if (q != MAP_FAILED) {
		munmap(q, G);                                /* free it for the COW */
		pid_t pid = fork();
		if (pid == 0) {
			for (unsigned long i = 0; i < G; i += STEP) p[i] = (char)(i / STEP + 1);
			for (unsigned long i = 0; i < G; i += STEP)
				if (p[i] != (char)(i / STEP + 1)) _exit(2);
			_exit(0);
		}
		int st = 0;
		waitpid(pid, &st, 0);
		if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { puts("FAIL child"); return 0; }
		for (unsigned long i = 0; i < G; i += STEP)
			if (p[i] != (char)(i / STEP)) { puts("FAIL parent-cow"); return 0; }
	}

	if (munmap(p, G) != 0) { puts("FAIL munmap"); return 0; }
	puts("OK");
	return 0;
}
