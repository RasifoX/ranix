/* Capstone: build a rootless container by composing all the namespaces.
 * New user ns (mapped root) → new uts/ipc/mount/net/cgroup → set hostname →
 * new pid ns whose first child is the container's pid 1. Inside, the process
 * is uid 0, pid 1, with its own hostname — a real container. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <sys/utsname.h>
#include <sys/wait.h>

int main(void)
{
	if (unshare(CLONE_NEWUSER) != 0) { printf("container-fail userns\n"); return 0; }
	int fd = open("/proc/self/uid_map", O_WRONLY);
	if (fd < 0) { printf("container-fail map\n"); return 0; }
	write(fd, "0 0 1\n", 6);     /* uid 0 inside == our real uid */
	close(fd);

	if (unshare(CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWCGROUP) != 0) {
		printf("container-fail unshare\n"); return 0;
	}
	sethostname("ranixctr", 8);

	if (unshare(CLONE_NEWPID) != 0) { printf("container-fail pidns\n"); return 0; }

	pid_t pid = fork();
	if (pid == 0) {
		/* container init */
		struct utsname u; uname(&u);
		int ok = (getpid() == 1) && (getuid() == 0) && (strcmp(u.nodename, "ranixctr") == 0);
		_exit(ok ? 0 : 2);
	}

	int st;
	waitpid(pid, &st, 0);
	printf("%s\n", (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? "container-ok" : "container-fail");
	return 0;
}
