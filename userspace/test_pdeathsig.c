/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* PR_SET_PDEATHSIG delivers its signal when the parent dies. A child relying on it
 * to go down with a killed parent otherwise runs on, and holds whatever it holds. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
	int p[2], go[2];
	if (pipe(p) || pipe(go)) { puts("FAIL pipe"); return 1; }
	pid_t mid = fork();
	if (mid == 0) {
		pid_t g = fork();
		if (g == 0) {
			close(p[0]); close(go[0]);
			prctl(PR_SET_PDEATHSIG, SIGKILL);
			write(go[1], "x", 1);
			for (;;) pause();           /* p[1] closes only when this dies */
		}
		char c;
		close(go[1]);
		read(go[0], &c, 1);
		_exit(0);
	}
	close(p[1]); close(go[1]); close(go[0]);
	waitpid(mid, NULL, 0);
	alarm(10);
	char c;
	if (read(p[0], &c, 1) != 0) { puts("FAIL data"); return 1; }
	alarm(0);
	puts("OK");
	return 0;
}
