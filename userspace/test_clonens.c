/* clone(CLONE_NEWUTS): the child starts in a fresh UTS namespace without
 * calling unshare, so its hostname change never reaches the parent. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>

int main(void)
{
	sethostname("p-host", 6);

	long pid = syscall(SYS_clone, CLONE_NEWUTS | 17 /*SIGCHLD*/, 0, 0, 0, 0);
	if (pid == 0) {
		sethostname("c-host", 6);
		struct utsname u;
		uname(&u);
		_exit(strcmp(u.nodename, "c-host") == 0 ? 0 : 3);
	}
	if (pid < 0) { printf("clonens-fail clone\n"); return 0; }

	int st;
	waitpid((pid_t)pid, &st, 0);

	struct utsname u;
	uname(&u);

	if (WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
	    strcmp(u.nodename, "p-host") == 0)
		printf("clonens-ok\n");
	else
		printf("clonens-fail child=%d host=%s\n",
		       WIFEXITED(st) ? WEXITSTATUS(st) : -1, u.nodename);
	return 0;
}
