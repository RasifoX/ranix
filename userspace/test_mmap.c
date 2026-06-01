/* mmap anonymous RW + mprotect enforcement. A child maps a page RW, writes it,
 * drops it to read-only, then writes again — which must fault. The parent
 * confirms the child was killed by SIGSEGV (not that it ran past the write,
 * which would mean mprotect was a no-op). Exercises mmap, mprotect PTE/VMA
 * downgrade, and the fault path's SIGSEGV exit status. */
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

int main(void)
{
	size_t pg = 4096;

	pid_t pid = fork();
	if (pid == 0) {
		char *p = mmap(NULL, pg, PROT_READ | PROT_WRITE,
		               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED) _exit(1);
		strcpy(p, "ranix");
		if (memcmp(p, "ranix", 5) != 0) _exit(2);
		if (mprotect(p, pg, PROT_READ) < 0) _exit(3);
		if (memcmp(p, "ranix", 5) != 0) _exit(4);   /* read must still work */
		p[0] = 'X';                                  /* RO write → must fault */
		_exit(42);                                   /* reached only if mprotect is a no-op */
	}

	int st = 0;
	waitpid(pid, &st, 0);
	if (WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV)
		puts("OK");
	else
		printf("FAIL st=0x%x\n", st);
	return 0;
}
