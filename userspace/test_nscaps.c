/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/syscall.h>

/* unshare(CLONE_NEWUSER) gives a namespace-local capability set. It must not let
 * an unprivileged process act on the machine. */
int main(void)
{
	pid_t c = fork();
	if (c == 0) {
		if (setuid(1000) != 0) _exit(10);
		if (unshare(CLONE_NEWUSER) != 0) _exit(11);
		struct timeval tv = { 1, 0 };
		if (settimeofday(&tv, NULL) == 0) _exit(12);
		if (syscall(SYS_reboot, 0xfee1dead, 0x28121969, 0, NULL) == 0) _exit(13);
		if (errno != EPERM) _exit(14);
		_exit(0);
	}
	int st = 0;
	waitpid(c, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0) { puts("OK"); return 0; }
	printf("FAIL %d\n", WEXITSTATUS(st));
	return 1;
}
