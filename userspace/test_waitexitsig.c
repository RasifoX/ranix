/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A child's exit and its exit signal arrive together: a wait sleeping for it
 * returns the child, not EINTR, even with a handler and no SA_RESTART
 * (LTP clone301). EINTR is only for a signal while nothing is waitable. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void on_sig(int s) { (void)s; }

int main(void)
{
	int bad = 0;
	struct sigaction sa = { .sa_handler = on_sig };   /* no SA_RESTART */
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	for (int i = 0; i < 20 && !bad; i++) {
		pid_t c = fork();
		if (c == 0) {
			struct timespec ts = { 0, 30 * 1000 * 1000 };
			nanosleep(&ts, NULL);   /* exit while the parent sleeps in wait */
			_exit(7);
		}
		int st = 0;
		pid_t w = i % 2 ? waitpid(-1, &st, 0) : waitpid(c, &st, 0);
		if (w != c || !WIFEXITED(st) || WEXITSTATUS(st) != 7) {
			printf("round %d: wait returned %d errno %d, want the child %d\n", i, w, errno, c); bad++;
		}
	}
	/* a signal with nothing to report still interrupts */
	pid_t c = fork();
	alarm(10);   /* a lost wakeup must fail, not hang */
	if (c == 0) {
		struct timespec ts = { 0, 100 * 1000 * 1000 };
		nanosleep(&ts, NULL);   /* let the parent sleep in wait first */
		kill(getppid(), SIGUSR1);
		pause();
		_exit(0);
	}
	errno = 0;
	pid_t w = waitpid(c, NULL, 0);
	if (w != -1 || errno != EINTR) { printf("SIGUSR1 while the child lives: %d errno %d, want EINTR\n", w, errno); bad++; }
	kill(c, SIGKILL);
	waitpid(c, NULL, 0);
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
