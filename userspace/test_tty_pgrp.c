/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* TIOCSPGRP accepts only a group of the caller's session on the caller's own
 * terminal, TIOCSCTTY cannot take a terminal another session holds, and the
 * console, where init's shell has no controlling terminal, keeps working. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/wait.h>

#ifndef TIOCGPTN
#define TIOCGPTN   0x80045430
#endif
#ifndef TIOCSPTLCK
#define TIOCSPTLCK 0x40045431
#endif

static int m, s;

static int set_fg(int fd, pid_t g)
{
	return ioctl(fd, TIOCSPGRP, &g) == 0 ? 0 : errno;
}

static pid_t get_fg(int fd)
{
	pid_t g = -1;
	ioctl(fd, TIOCGPGRP, &g);
	return g;
}

static pid_t outsider;   /* a session and group of its own */

/* session that owns the pty */
static int holder(int gate)
{
	char b;
	if (setsid() < 0) return 1;
	if (ioctl(s, TIOCSCTTY, 0)) return 2;
	pid_t me = getpid();
	if (get_fg(s) != me) return 3;

	pid_t bg = fork();
	if (bg == 0) { prctl(PR_SET_PDEATHSIG, SIGKILL); setpgid(0, 0); for (;;) pause(); }
	setpgid(bg, bg);

	if (set_fg(s, bg)) return 4;                  /* same session */
	if (get_fg(s) != bg) return 5;
	if (set_fg(s, me)) return 6;
	if (set_fg(s, outsider) != EPERM) return 7;   /* another session */
	if (set_fg(s, 99999) != ESRCH) return 8;
	if (set_fg(s, -1) != EINVAL) return 9;
	if (get_fg(s) != me) return 10;

	/* a member that is not the leader cannot take a terminal */
	pid_t k = fork();
	if (k == 0) {
		int fd = open("/dev/ptmx", O_RDWR);
		_exit(ioctl(fd, TIOCSCTTY, 0) == -1 && errno == EPERM ? 0 : 1);
	}
	int st;
	waitpid(k, &st, 0);
	if (WEXITSTATUS(st)) return 11;

	write(gate, "x", 1);    /* the parent now probes from outside */
	read(gate, &b, 1);
	kill(bg, SIGKILL);
	waitpid(bg, NULL, 0);
	return 0;
}

/* no terminal of its own, or another session's */
static int stranger(void)
{
	if (setsid() < 0) return 1;
	if (set_fg(s, getpid()) != ENOTTY) return 2;
	if (ioctl(s, TIOCSCTTY, 0) != -1 || errno != EPERM) return 3;
	if (setuid(64002)) return 4;
	if (ioctl(s, TIOCSCTTY, 1) != -1 || errno != EPERM) return 5;
	return 0;
}

static int console(void)
{
	int fd = open("/dev/console", O_RDWR | O_NOCTTY);
	if (fd < 0) return 1;
	pid_t was = get_fg(fd);

	/* what init does before it execs the console shell */
	pid_t c = fork();
	if (c == 0) {
		setsid();
		pid_t me = getpid();
		if (set_fg(fd, me)) _exit(2);
		if (get_fg(fd) != me) _exit(3);
		if (set_fg(fd, outsider) != EPERM) _exit(4);
		_exit(0);
	}
	int st;
	waitpid(c, &st, 0);
	if (!WIFEXITED(st)) return 20;
	if (WEXITSTATUS(st)) return 10 + WEXITSTATUS(st);

	/* a real shell on that path, with job control */
	int pp[2];
	pipe(pp);
	c = fork();
	if (c == 0) {
		alarm(8);
		setsid();
		dup2(fd, 0);
		dup2(pp[1], 1);
		dup2(pp[1], 2);
		close(pp[0]);
		pid_t me = getpid();
		ioctl(0, TIOCSPGRP, &me);
		execl("/bin/sh", "sh", "-ic", "/bin/echo fg; /bin/sleep 0 & wait; echo console-shell-ok", (char *)NULL);
		_exit(127);
	}
	close(pp[1]);
	char out[512];
	int n = 0, r;
	while (n < (int)sizeof(out) - 1 && (r = read(pp[0], out + n, sizeof(out) - 1 - n)) > 0) n += r;
	out[n] = 0;
	close(pp[0]);
	waitpid(c, &st, 0);

	/* the group is gone with the child; the caller's own session gets it back */
	ioctl(fd, TIOCSPGRP, &was);
	close(fd);
	if (!strstr(out, "console-shell-ok")) { printf("shell said: %s\n", out); return 2; }
	return 0;
}

int main(void)
{
	m = open("/dev/ptmx", O_RDWR);
	if (m < 0) { puts("FAIL ptmx"); return 1; }
	int n = -1, zero = 0;
	ioctl(m, TIOCGPTN, &n);
	ioctl(m, TIOCSPTLCK, &zero);
	char path[32];
	snprintf(path, sizeof(path), "/dev/pts/%d", n);
	s = open(path, O_RDWR | O_NOCTTY);
	if (s < 0) { puts("FAIL pts"); return 1; }

	int p[2];
	pipe(p);
	outsider = fork();
	if (outsider == 0) {
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		setsid();
		write(p[1], "x", 1);
		for (;;) pause();
	}
	char b;
	read(p[0], &b, 1);

	int g[2], st;
	socketpair(1 /* AF_UNIX */, 1 /* SOCK_STREAM */, 0, g);
	pid_t h = fork();
	if (h == 0) _exit(holder(g[1]));
	close(g[1]);
	if (read(g[0], &b, 1) != 1) { waitpid(h, &st, 0); printf("FAIL holder rc=%d\n", WEXITSTATUS(st)); return 1; }

	pid_t c = fork();
	if (c == 0) _exit(stranger());
	waitpid(c, &st, 0);
	int r = WEXITSTATUS(st);
	write(g[0], "x", 1);
	waitpid(h, &st, 0);
	if (r) { printf("FAIL stranger rc=%d\n", r); return 1; }
	if (WEXITSTATUS(st)) { printf("FAIL holder rc=%d\n", WEXITSTATUS(st)); return 1; }
	printf("pty ok\n");

	/* the session ended, so the terminal is free again */
	c = fork();
	if (c == 0) {
		setsid();
		_exit(ioctl(s, TIOCSCTTY, 0) ? 1 : 0);
	}
	waitpid(c, &st, 0);
	if (WEXITSTATUS(st)) { printf("FAIL reclaim\n"); return 1; }
	printf("reclaim ok\n");

	r = console();
	if (r) { printf("FAIL console rc=%d\n", r); return 1; }
	printf("console ok\n");
	kill(outsider, SIGKILL);
	printf("OK\n");
	return 0;
}
