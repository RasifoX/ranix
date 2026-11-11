/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A USB serial port as a terminal, in the test VM: QEMU's usb-serial (FTDI) with a
 * FIFO that loops every byte back, so what the port sends it also receives.
 * Checks canonical editing (^? erases, ^U kills the line, ^D ends a line, ^D
 * alone is end of file), echo (a byte sent with ECHO on keeps coming back), the
 * port as a controlling terminal (TIOCSCTTY, TIOCGPGRP) and ^C reaching its
 * foreground group as SIGINT, and break (TIOCSBRK, TIOCCBRK, tcsendbreak).
 * Prints OK, or the first step that failed. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#define DEV "/dev/ttyUSB0"

static int fail(const char *step)
{
	printf("serialtty-fail %s errno=%d\n", step, errno);
	return 1;
}

/* canonical, no echo, no signals unless asked */
static int mode_when(int fd, tcflag_t lflag, int when)
{
	struct termios t;
	if (tcgetattr(fd, &t)) return -1;
	cfmakeraw(&t);
	cfsetspeed(&t, B115200);
	t.c_lflag = lflag;
	t.c_cc[VERASE] = 0x7F; t.c_cc[VKILL] = 0x15; t.c_cc[VEOF] = 0x04; t.c_cc[VINTR] = 0x03;
	t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
	return tcsetattr(fd, when, &t);
}

static int mode(int fd, tcflag_t lflag) { return mode_when(fd, lflag, TCSAFLUSH); }

static int rd(int fd, char *b, int n)
{
	struct pollfd p = { fd, POLLIN, 0 };
	if (poll(&p, 1, 3000) != 1) return -2;
	return (int)read(fd, b, (size_t)n);
}

static int t_canon(int fd)
{
	char b[64];
	if (mode(fd, ICANON)) return fail("canon-mode");
	if (write(fd, "abx\x7f" "c\n", 6) != 6) return fail("w1");
	int n = rd(fd, b, sizeof b);
	if (n != 4 || memcmp(b, "abc\n", 4)) return printf("serialtty-fail erase %d\n", n), 1;
	if (write(fd, "zz\x15ok\n", 6) != 6) return fail("w2");
	n = rd(fd, b, sizeof b);
	if (n != 3 || memcmp(b, "ok\n", 3)) return printf("serialtty-fail kill %d\n", n), 1;
	if (write(fd, "ab\x04", 3) != 3) return fail("w3");
	n = rd(fd, b, sizeof b);
	if (n != 2 || memcmp(b, "ab", 2)) return printf("serialtty-fail eof-line %d\n", n), 1;
	if (write(fd, "\x04", 1) != 1) return fail("w4");
	n = rd(fd, b, sizeof b);
	if (n != 0) return printf("serialtty-fail eof %d\n", n), 1;
	return 0;
}

static int t_echo(int fd)
{
	char b[4096];
	if (mode(fd, ECHO)) return fail("echo-mode");
	if (write(fd, "e", 1) != 1) return fail("echo-write");
	usleep(200000);                          /* it goes round: out, back, echoed out again */
	if (mode_when(fd, 0, TCSANOW)) return fail("echo-off");   /* keep what came round */
	usleep(100000);
	int total = 0, n;
	struct pollfd p = { fd, POLLIN, 0 };
	while (poll(&p, 1, 200) == 1 && (n = (int)read(fd, b, sizeof b)) > 0) {
		for (int i = 0; i < n; i++) if (b[i] != 'e') return printf("serialtty-fail echo byte %02x\n", b[i]), 1;
		total += n;
	}
	if (total < 2) return printf("serialtty-fail echo count %d\n", total), 1;
	return 0;
}

static volatile sig_atomic_t got_int;
static void on_int(int s) { (void)s; got_int = 1; }

/* A child in its own session takes the port as its terminal and sends ^C round. */
static int t_isig(void)
{
	int pfd[2];
	if (pipe(pfd)) return fail("pipe");
	pid_t pid = fork();
	if (pid == 0) {
		char r = 'x';
		signal(SIGINT, on_int);
		if (setsid() < 0) r = '1';
		int fd = open(DEV, O_RDWR | O_NOCTTY);
		pid_t pg = 0;
		if (r == 'x' && (fd < 0 || ioctl(fd, TIOCSCTTY, 0))) r = '2';
		if (r == 'x' && (ioctl(fd, TIOCGPGRP, &pg) || pg != getpgrp())) r = '3';
		if (r == 'x' && mode(fd, ISIG | ICANON)) r = '4';
		if (r == 'x' && write(fd, "\x03", 1) != 1) r = '5';
		for (int i = 0; r == 'x' && i < 300 && !got_int; i++) usleep(10000);
		if (r == 'x') r = got_int ? 'k' : '6';
		if (write(pfd[1], &r, 1) != 1) _exit(1);
		_exit(0);
	}
	close(pfd[1]);
	char r = 0;
	if (read(pfd[0], &r, 1) != 1) r = '?';
	waitpid(pid, 0, 0);
	close(pfd[0]);
	if (r != 'k') return printf("serialtty-fail isig step %c\n", r), 1;
	return 0;
}

static int t_break(int fd)
{
	if (ioctl(fd, TIOCSBRK) || ioctl(fd, TIOCCBRK)) return fail("tiocsbrk");
	if (tcsendbreak(fd, 0)) return fail("tcsendbreak");
	return 0;
}

int main(void)
{
	int fd = open(DEV, O_RDWR | O_NOCTTY);
	if (fd < 0) return fail("open"), 0;
	if (t_canon(fd) || t_echo(fd) || t_break(fd)) return 0;
	close(fd);
	if (t_isig()) return 0;
	printf("OK\n");
	return 0;
}
