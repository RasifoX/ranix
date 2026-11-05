/* cgroup.procs: the pid is read in the writer's pid namespace, an open fd is not
 * enough to move someone else's process, and 0 means the writer. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define PROCS "/cgroup/cperm/cgroup.procs"

static int put(int fd, long v)
{
	char b[32];
	int n = sprintf(b, "%ld\n", v);
	return write(fd, b, n);
}

static int member(long pid)
{
	char b[256], *s;
	int fd = open(PROCS, O_RDONLY);
	int n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0) return 0;
	b[n] = 0;
	for (s = strtok(b, "\n"); s; s = strtok(NULL, "\n"))
		if (atol(s) == pid) return 1;
	return 0;
}

#define FAIL(what) do { printf("FAIL %s\n", what); return 1; } while (0)

int main(void)
{
	mkdir("/cgroup", 0755);
	mount("none", "/cgroup", "cgroup2", 0, NULL);
	mkdir("/cgroup/cperm", 0755);
	int fd = open(PROCS, O_WRONLY);
	if (fd < 0) FAIL("open");

	pid_t victim = fork();
	if (victim == 0) { prctl(PR_SET_PDEATHSIG, SIGKILL); for (;;) pause(); }

	/* unprivileged holder of a root-opened fd: not the victim's owner */
	pid_t c = fork();
	if (c == 0) {
		if (setuid(64001)) _exit(9);
		if (put(fd, victim) != -1 || errno != EACCES) _exit(1);
		if (put(fd, 1) != -1 || errno != EACCES) _exit(2);
		if (put(fd, 0) < 0 || !member(getpid())) _exit(3);
		_exit(0);
	}
	int st;
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("FAIL uid rc=%d\n", WEXITSTATUS(st)); kill(victim, SIGKILL); return 1; }
	if (member(victim)) FAIL("victim moved");
	printf("uid ok\n");

	/* root may move anyone; a missing pid is ESRCH */
	if (put(fd, victim) < 0 || !member(victim)) FAIL("root move");
	if (put(fd, 99999) != -1 || errno != ESRCH) FAIL("esrch");
	printf("root ok\n");
	kill(victim, SIGKILL);
	waitpid(victim, NULL, 0);

	/* inside a pid namespace numbers are local: 1 is the namespace's init, and the
	 * global pid of a process outside it does not resolve */
	pid_t me = getpid();
	c = syscall(SYS_clone, CLONE_NEWPID | SIGCHLD, 0, 0, 0, 0);
	if (c == 0) {
		if (put(fd, me) != -1 || errno != ESRCH) _exit(1);
		if (put(fd, 1) < 0) _exit(2);
		_exit(0);
	}
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("FAIL pidns rc=%d\n", WEXITSTATUS(st)); return 1; }
	if (member(me)) FAIL("pidns reached outside");
	printf("pidns ok\nOK\n");
	return 0;
}
