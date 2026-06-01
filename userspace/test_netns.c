/* network namespace: a child that unshares CLONE_NEWNET gets an isolated
 * network stack (its own loopback), so its /proc/self/ns/net identity differs
 * from the parent's. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/wait.h>

int main(void)
{
	char parent_ns[64] = {0};
	readlink("/proc/self/ns/net", parent_ns, sizeof(parent_ns) - 1);

	pid_t pid = fork();
	if (pid == 0) {
		if (unshare(CLONE_NEWNET) != 0) _exit(2);
		char child_ns[64] = {0};
		readlink("/proc/self/ns/net", child_ns, sizeof(child_ns) - 1);
		_exit(strcmp(parent_ns, child_ns) != 0 ? 0 : 3);   /* must differ */
	}

	int st;
	waitpid(pid, &st, 0);
	printf("%s\n", (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? "netns-ok" : "netns-fail");
	return 0;
}
