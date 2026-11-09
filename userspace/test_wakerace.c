/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A signal landing just before a task goes to sleep must not be lost, and a task
 * stopped by ptrace while running in user mode must come back from the stop whole:
 * that one used to reset the machine. The windows are a few instructions wide, so
 * this runs them many times. A loss hangs in waitpid; the alarm reports it. */
#define _GNU_SOURCE
#include <signal.h>
#include <sys/ptrace.h>
#include "asess_test.h"

#define ROUNDS 1000

static void on_alarm(int s)
{
	(void)s;
	static const char m[] = "FAILED lost wakeup\n";
	(void)!write(1, m, sizeof(m) - 1);
	_exit(1);
}

/* jitter so the kill walks across the target's path into sleep */
static void spin(int n)
{
	for (volatile int i = 0; i < n; i++) {}
}

static void pause_race(int rounds)
{
	for (int r = 0; r < rounds; r++) {
		int p[2];
		CHECK(pipe(p) == 0, "pipe");
		pid_t c = fork();
		if (c == 0) {
			close(p[0]);
			(void)!write(p[1], "x", 1);
			for (;;) pause();
		}
		char b;
		close(p[1]);
		(void)!read(p[0], &b, 1);
		close(p[0]);
		spin((r * 37) % 4000);
		kill(c, SIGKILL);
		alarm(20);
		CHECK(waitpid(c, NULL, 0) == c, "waitpid");
		alarm(0);
	}
}

static void ptrace_race(int rounds)
{
	for (int r = 0; r < rounds; r++) {
		pid_t c = fork();
		if (c == 0) for (;;) pause();
		spin((r * 53) % 2000);
		CHECK(ptrace(PTRACE_ATTACH, c, 0, 0) == 0, "attach");
		spin((r * 91) % 20000);
		kill(c, SIGKILL);
		alarm(20);
		int st;
		pid_t w;
		do w = waitpid(c, &st, 0); while (w == c && WIFSTOPPED(st));
		CHECK(w == c, "waitpid");
		alarm(0);
	}
}

int main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : ROUNDS;
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGALRM, on_alarm);
	pause_race(rounds);
	puts("pause ok");
	ptrace_race(rounds);
	puts("ptrace ok");
	return finish();
}
