/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* setns into a mnt, net, pid or user namespace takes CAP_SYS_ADMIN in the initial
 * user namespace: a namespace fd alone is no way in. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

static const char *names[] = { "mnt", "net", "pid", "user" };
static int fds[4];

static int run(uid_t uid, int userns, int want_ok)
{
	pid_t c = fork();
	if (c == 0) {
		if (uid && setuid(uid)) _exit(10);
		if (userns && unshare(CLONE_NEWUSER)) _exit(11);
		for (int i = 0; i < 4; i++) {
			int r = setns(fds[i], 0);
			if (want_ok ? r != 0 : (r != -1 || errno != EPERM)) _exit(i + 1);
		}
		_exit(0);
	}
	int st = -1;
	waitpid(c, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}

int main(void)
{
	for (int i = 0; i < 4; i++) {
		char p[32];
		snprintf(p, sizeof(p), "/proc/self/ns/%s", names[i]);
		if ((fds[i] = open(p, O_RDONLY)) < 0) { printf("FAIL open %s\n", p); return 1; }
	}
	int a = run(1000, 0, 0), b = run(1000, 1, 0), c = run(0, 1, 0), d = run(0, 0, 1);
	if (a || b || c || d) { printf("FAIL user=%d userns=%d rootns=%d root=%d\n", a, b, c, d); return 1; }
	puts("OK");
	return 0;
}
