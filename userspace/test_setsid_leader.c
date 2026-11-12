/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* setsid() by a process group leader fails with EPERM (POSIX, Linux): the new
 * session would leave the old group's members in a group whose id is now a
 * session of its own. Covers the leader of a fresh session (a second setsid),
 * a process that made itself a group leader with setpgid(0, 0), and keeps the
 * plain case working (a child that leads nothing gets a new session). */
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

static int child(int how)
{
	pid_t c = fork();
	if (c == 0) {
		if (how == 0) _exit(setsid() == getpid() && getsid(0) == getpid() ? 0 : 1);
		if (how == 1) {
			if (setsid() != getpid()) _exit(2);
			errno = 0;
			_exit(setsid() < 0 && errno == EPERM ? 0 : 3);
		}
		if (setpgid(0, 0) != 0) _exit(4);
		pid_t sid = getsid(0);
		errno = 0;
		if (!(setsid() < 0 && errno == EPERM)) _exit(5);
		_exit(getsid(0) == sid && getpgrp() == getpid() ? 0 : 6);
	}
	int st = 0;
	if (c < 0 || waitpid(c, &st, 0) != c || !WIFEXITED(st)) return 99;
	return WEXITSTATUS(st);
}

int main(void)
{
	int r;
	if ((r = child(0)) != 0) { printf("FAIL plain %d\n", r); return 0; }
	if ((r = child(1)) != 0) { printf("FAIL twice %d\n", r); return 0; }
	if ((r = child(2)) != 0) { printf("FAIL pgrp-leader %d\n", r); return 0; }
	puts("OK");
	return 0;
}
