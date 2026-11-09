/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/syscall.h>

/* Another uid's process cannot be reconfigured; one's own can. */
int main(void)
{
	pid_t target = fork();
	if (target == 0) { pause(); _exit(0); }
	pid_t c = fork();
	if (c == 0) {
		if (setuid(1000) != 0) _exit(10);
		struct rlimit r = { 1, 1 };
		if (prlimit(target, RLIMIT_NOFILE, &r, NULL) == 0 || errno != EPERM) _exit(11);
		struct sched_param sp = { 0 };
		if (syscall(SYS_sched_setscheduler, target, SCHED_OTHER, &sp) == 0 || errno != EPERM) _exit(12);
		if (syscall(SYS_sched_setparam, target, &sp) == 0 || errno != EPERM) _exit(13);
		cpu_set_t s; CPU_ZERO(&s); CPU_SET(0, &s);
		if (sched_setaffinity(target, sizeof(s), &s) == 0 || errno != EPERM) _exit(14);
		if (prlimit(getpid(), RLIMIT_NOFILE, NULL, &r) != 0) _exit(15);
		_exit(0);
	}
	int st = 0;
	waitpid(c, &st, 0);
	kill(target, SIGKILL); waitpid(target, NULL, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0) { puts("OK"); return 0; }
	printf("FAIL %d\n", WEXITSTATUS(st));
	return 1;
}
