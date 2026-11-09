/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* SA_RESETHAND and SA_NODEFER. A handler that re-raises its own signal is how a
 * process dies by that signal after cleaning up; Node does this for SIGTERM.
 * First failure or OK. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile int runs, depth, maxdepth, calls;

static void reraise(int sig)
{
	runs++;
	raise(sig);
}

static void nested(int sig)
{
	if (++depth > maxdepth) maxdepth = depth;
	if (++calls < 3) raise(sig);   /* a blocked one lands after this return */
	depth--;
}

int main(void)
{
	pid_t pid = fork();
	if (pid == 0) {
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = reraise;
		sa.sa_flags = (int)SA_RESETHAND;
		sigaction(SIGTERM, &sa, NULL);
		raise(SIGTERM);
		_exit(runs == 1 ? 7 : 8);
	}
	int st;
	alarm(5);
	waitpid(pid, &st, 0);
	if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGTERM) { printf("FAIL resethand st=0x%x\n", st); return 1; }

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = nested;
	sa.sa_flags = SA_NODEFER;
	sigaction(SIGUSR1, &sa, NULL);
	raise(SIGUSR1);
	if (maxdepth != 3) { printf("FAIL nodefer depth %d\n", maxdepth); return 1; }

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = nested;
	sigaction(SIGUSR2, &sa, NULL);
	maxdepth = 0;
	calls = 0;
	raise(SIGUSR2);
	if (maxdepth != 1) { printf("FAIL plain handler nested, depth %d\n", maxdepth); return 1; }
	puts("OK");
	return 0;
}
