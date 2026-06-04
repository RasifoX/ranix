/* OOM survival: a child mmaps and touches anonymous memory until the kernel
 * can't satisfy it — either the OOM killer SIGKILLs the child (it is the
 * biggest user process) or mmap/the fault returns ENOMEM/SIGSEGV. Either way
 * the child must die, the kernel must NOT panic, and the parent must keep
 * running and still be able to allocate afterwards. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#define CHUNK   (64UL * 1024 * 1024)   /* 64 MB per mmap */
#define MAXMEM  (16UL * 1024 * 1024 * 1024)  /* stop after 16 GB of attempts */

int main(void)
{
	pid_t pid = fork();
	if (pid == 0) {
		unsigned long total = 0;
		while (total < MAXMEM) {
			char *m = mmap(0, CHUNK, PROT_READ | PROT_WRITE,
			               MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
			if (m == MAP_FAILED) _exit(1);   /* graceful ENOMEM */
			for (unsigned long i = 0; i < CHUNK; i += 4096)
				m[i] = 1;                    /* fault each page in */
			total += CHUNK;
		}
		_exit(2);   /* never hit OOM nor ENOMEM */
	}

	int st = 0;
	waitpid(pid, &st, 0);

	int died = (WIFSIGNALED(st) && (WTERMSIG(st) == SIGKILL || WTERMSIG(st) == SIGSEGV))
	         || (WIFEXITED(st) && WEXITSTATUS(st) == 1);
	if (!died) { printf("FAIL child st=0x%x\n", st); return 0; }

	/* System survived — prove the parent can still allocate, touch and free. */
	char *p = mmap(0, CHUNK, PROT_READ | PROT_WRITE,
	               MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (p == MAP_FAILED) { printf("FAIL parent mmap\n"); return 0; }
	for (unsigned long i = 0; i < CHUNK; i += 4096) p[i] = 2;
	if (p[0] != 2 || p[CHUNK - 4096] != 2) { printf("FAIL parent rw\n"); return 0; }
	munmap(p, CHUNK);

	puts("OK");
	return 0;
}
