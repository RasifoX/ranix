/* UTS namespace isolation: a child that unshares CLONE_NEWUTS can change
 * its hostname without the parent (different namespace) ever seeing it. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/utsname.h>
#include <sys/wait.h>

int main(void)
{
	sethostname("parent-host", 11);

	pid_t pid = fork();
	if (pid == 0) {
		char ns_before[64] = {0}, ns_after[64] = {0};
		readlink("/proc/self/ns/uts", ns_before, sizeof(ns_before) - 1);
		if (unshare(CLONE_NEWUTS) != 0) _exit(2);
		readlink("/proc/self/ns/uts", ns_after, sizeof(ns_after) - 1);
		sethostname("child-host", 10);
		struct utsname u;
		uname(&u);
		int ok = strcmp(u.nodename, "child-host") == 0 &&
		         strcmp(ns_before, ns_after) != 0;
		_exit(ok ? 0 : 3);
	}

	int st;
	waitpid(pid, &st, 0);

	struct utsname u;
	uname(&u);

	if (WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
	    strcmp(u.nodename, "parent-host") == 0)
		printf("uts-ok\n");
	else
		printf("uts-fail child=%d host=%s\n",
		       WIFEXITED(st) ? WEXITSTATUS(st) : -1, u.nodename);
	return 0;
}
