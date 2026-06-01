/* pid namespace: a child created with CLONE_NEWPID is pid 1 (init) of the new
 * namespace, so getpid() returns 1 there while the parent still sees it by its
 * own (global) pid. */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/wait.h>

int main(void)
{
	long pid = syscall(SYS_clone, CLONE_NEWPID | 17 /*SIGCHLD*/, 0, 0, 0, 0);
	if (pid == 0) {
		_exit(getpid() == 1 ? 0 : 3);   /* nr 1 in the new pid namespace */
	}
	if (pid < 0) { printf("pidns-fail clone\n"); return 0; }

	int st;
	waitpid((pid_t)pid, &st, 0);
	printf("%s\n", (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? "pidns-ok" : "pidns-fail");
	return 0;
}
