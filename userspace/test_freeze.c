/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* cgroup freezer: a child parked in a frozen cgroup makes no progress (cannot
 * exit) until the cgroup is thawed. The child blocks on a pipe; the parent
 * freezes the cgroup, then unblocks it — a frozen child stays put, and only
 * runs to exit after thaw. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

static void cgwrite(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);
	if (fd >= 0) { write(fd, s, strlen(s)); close(fd); }
}

int main(void)
{
	mkdir("/cgroup/ftest", 0755);
	int pfd[2];
	if (pipe(pfd) != 0) { printf("freeze-fail pipe\n"); return 0; }

	pid_t pid = fork();
	if (pid == 0) {
		char c; read(pfd[0], &c, 1);   /* block until parent unblocks */
		_exit(42);
	}

	char b[16]; int n = sprintf(b, "%d\n", (int)pid);
	cgwrite("/cgroup/ftest/cgroup.procs", b);   /* move child into the cgroup */
	cgwrite("/cgroup/ftest/cgroup.freeze", "1"); /* freeze */
	write(pfd[1], "x", 1);                       /* unblock the child's read */

	sleep(1);
	int st;
	int r1 = waitpid(pid, &st, WNOHANG);   /* frozen → must NOT have exited */

	cgwrite("/cgroup/ftest/cgroup.freeze", "0"); /* thaw */
	int r2 = waitpid(pid, &st, 0);               /* now it runs to exit */

	if (!(r1 == 0 && r2 == pid && WIFEXITED(st) && WEXITSTATUS(st) == 42)) {
		printf("freeze-fail r1=%d r2=%d\n", r1, r2);
		return 0;
	}

	/* A child that has not gone to sleep yet is frozen too: it finds the byte
	 * without ever entering the scheduler, and must stop before user mode. */
	for (int i = 0; i < 100; i++) {
		int q[2];
		pipe(q);
		pid = fork();
		if (pid == 0) {
			char c; read(q[0], &c, 1);
			_exit(42);
		}
		sprintf(b, "%d\n", (int)pid);
		cgwrite("/cgroup/ftest/cgroup.procs", b);
		cgwrite("/cgroup/ftest/cgroup.freeze", "1");
		write(q[1], "x", 1);
		usleep(3000);
		r1 = waitpid(pid, &st, WNOHANG);
		cgwrite("/cgroup/ftest/cgroup.freeze", "0");
		r2 = r1 ? pid : waitpid(pid, &st, 0);
		close(q[0]); close(q[1]);
		if (r1 != 0) { printf("freeze-fail round %d r1=%d\n", i, r1); return 0; }
	}
	(void)r2;
	printf("freeze-ok\n");
	return 0;
}
