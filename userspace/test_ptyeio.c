/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A pty master with no slave open: what is buffered reads out first, then a read
 * fails with EIO, as on Linux (Python's pty module, script, tmux and sshd all
 * take EIO as "the terminal is gone"); it used to return 0. poll() says POLLHUP. */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>

#ifndef TIOCGPTN
#define TIOCGPTN  0x80045430
#endif
#ifndef TIOCSPTLCK
#define TIOCSPTLCK 0x40045431
#endif

int main(void)
{
	int m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
	if (m < 0) { puts("FAIL ptmx"); return 0; }
	int n = -1, zero = 0;
	if (ioctl(m, TIOCGPTN, &n) != 0 || n < 0) { puts("FAIL ptn"); return 0; }
	ioctl(m, TIOCSPTLCK, &zero);
	char path[32];
	snprintf(path, sizeof(path), "/dev/pts/%d", n);
	int s = open(path, O_RDWR | O_NOCTTY);
	if (s < 0) { puts("FAIL pts-open"); return 0; }
	struct termios tio;
	if (tcgetattr(s, &tio) == 0) { cfmakeraw(&tio); tcsetattr(s, TCSANOW, &tio); }
	if (write(s, "bye", 3) != 3) { puts("FAIL s-write"); return 0; }
	close(s);

	struct pollfd pf = { m, POLLIN, 0 };
	if (poll(&pf, 1, 1000) != 1) { puts("FAIL poll"); return 0; }
	char buf[16];
	ssize_t r = read(m, buf, sizeof(buf));
	if (r != 3 || memcmp(buf, "bye", 3) != 0) { printf("FAIL buffered r=%zd\n", r); return 0; }
	pf.revents = 0;
	if (poll(&pf, 1, 1000) != 1 || !(pf.revents & POLLHUP)) { printf("FAIL pollhup rev=%#x\n", pf.revents); return 0; }
	errno = 0;
	r = read(m, buf, sizeof(buf));
	if (!(r < 0 && errno == EIO)) { printf("FAIL eio r=%zd errno=%d\n", r, errno); return 0; }
	close(m);
	puts("OK");
	return 0;
}
