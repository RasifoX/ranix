/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Closing a pty master is a hangup: the foreground group gets SIGHUP before a
 * reader blocked on the slave is woken. Woken first, the reader saw EOF and went
 * on with its life (c-jobctl "FAIL hangup", once in a while, and every time with
 * a slow console log). A test kernel (ranix.test_hooks, PR_RANIX_TEST_HOOK bit
 * 2) holds master_close for 200 ms between its two steps, so a wrong order fails
 * here every time: the reader then exits 99 instead of dying of SIGHUP. */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/wait.h>

#ifndef TIOCGPTN
#define TIOCGPTN  0x80045430
#endif
#ifndef TIOCSPTLCK
#define TIOCSPTLCK 0x40045431
#endif
#define PR_RANIX_TEST_HOOK 0x52580020
#define HOOK_PTY_HANGUP    2

int main(void)
{
	if (prctl(PR_RANIX_TEST_HOOK, HOOK_PTY_HANGUP, 0, 0, 0) != 0) {
		puts("FAIL PR_RANIX_TEST_HOOK: boot with ranix.test_hooks"); return 0;
	}
	int m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
	if (m < 0) { puts("FAIL ptmx"); return 0; }
	int n = -1, zero = 0;
	if (ioctl(m, TIOCGPTN, &n) != 0 || n < 0) { puts("FAIL ptn"); return 0; }
	ioctl(m, TIOCSPTLCK, &zero);
	char path[32];
	snprintf(path, sizeof(path), "/dev/pts/%d", n);
	int s = open(path, O_RDWR | O_NOCTTY);
	if (s < 0) { puts("FAIL pts-open"); return 0; }

	int g[2]; if (pipe(g) != 0) { puts("FAIL pipe"); return 0; }
	pid_t c = fork();
	if (c == 0) {
		close(m); close(g[0]);
		setsid();
		if (ioctl(s, TIOCSCTTY, 0) != 0) _exit(98);
		signal(SIGHUP, SIG_DFL);
		write(g[1], "g", 1);
		alarm(10);
		char b[4];
		read(s, b, sizeof(b));      /* blocks until the hangup, which must kill us */
		_exit(99);
	}
	if (c < 0) { puts("FAIL fork"); return 0; }
	close(g[1]);
	char b; read(g[0], &b, 1);
	usleep(100000);                 /* the child is asleep in read */
	close(s);
	close(m);                       /* last master fd: hangup */
	int st = 0;
	if (waitpid(c, &st, 0) != c) { puts("FAIL wait"); return 0; }
	if (!WIFSIGNALED(st) || WTERMSIG(st) != SIGHUP) {
		printf("FAIL reader-woke-before-sighup %s=%d\n", WIFSIGNALED(st) ? "sig" : "exit",
		       WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
		return 0;
	}
	puts("OK");
	return 0;
}
