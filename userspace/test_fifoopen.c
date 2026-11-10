/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A FIFO reader blocked in open() returns once a writer has opened, even if
 * that writer already closed again before the reader ran (Linux counts the
 * writer opens; Ranix waited for writers > 0 and slept on forever). 300 times:
 * a child opens the FIFO for writing, writes a byte, closes and exits, while
 * the parent opens it for reading; an alarm ends an open that hangs. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define FIFO "/tmp/fifoopen_t"

static void on_alarm(int s) { (void)s; }

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_alarm;           /* no SA_RESTART: the alarm ends a hung open */
	sigaction(SIGALRM, &sa, NULL);
	int hung = 0, other = 0;
	for (int i = 0; i < 300; i++) {
		unlink(FIFO);
		if (mkfifo(FIFO, 0600)) { printf("mkfifo: errno %d\n", errno); puts("FAIL"); return 1; }
		pid_t c = fork();
		if (c == 0) {
			int w = open(FIFO, O_WRONLY);
			if (w < 0) _exit(2);
			if (write(w, "x", 1) != 1) _exit(3);
			close(w);
			_exit(0);
		}
		alarm(2);
		int r = open(FIFO, O_RDONLY);
		alarm(0);
		if (r < 0) { if (errno == EINTR) hung++; else other++; }
		else close(r);
		kill(c, SIGKILL);
		waitpid(c, NULL, 0);
	}
	unlink(FIFO);
	printf("300 FIFO open rendezvous: %d hung, %d other errors\n", hung, other);
	if (hung || other) { puts("FAIL"); return 1; }
	puts("OK");
	return 0;
}
