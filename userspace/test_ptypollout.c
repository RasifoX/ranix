/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* poll()/select() on a pty say POLLOUT when a write would not block, as on
 * Linux. sshd waits for its pty master to be writable before it hands the
 * client's keystrokes on; without POLLOUT they never reached the shell. A slave
 * is writable while there is room toward the master, and stops being so when
 * the master's buffer is full. */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>

#ifndef TIOCGPTN
#define TIOCGPTN  0x80045430
#endif
#ifndef TIOCSPTLCK
#define TIOCSPTLCK 0x40045431
#endif

static short ev(int fd, short want)
{
	struct pollfd pf = { fd, want, 0 };
	return poll(&pf, 1, 0) < 0 ? -1 : pf.revents;
}

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

	short r;
	if (!((r = ev(m, POLLOUT)) & POLLOUT)) { printf("FAIL master-pollout rev=%#x\n", r); return 0; }
	if ((r = ev(m, POLLIN)) & POLLIN)      { printf("FAIL master-pollin-empty rev=%#x\n", r); return 0; }
	if (!((r = ev(s, POLLOUT)) & POLLOUT)) { printf("FAIL slave-pollout rev=%#x\n", r); return 0; }

	fd_set w; FD_ZERO(&w); FD_SET(m, &w);
	struct timeval tv = { 0, 0 };
	if (select(m + 1, NULL, &w, NULL, &tv) != 1 || !FD_ISSET(m, &w)) { puts("FAIL master-select-write"); return 0; }

	/* fill the slave→master direction: the slave is no longer writable */
	fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
	char buf[512] = { 'x' };
	while (write(s, buf, sizeof(buf)) > 0) ;
	if ((r = ev(s, POLLOUT)) & POLLOUT)    { printf("FAIL slave-full-pollout rev=%#x\n", r); return 0; }
	char rb[1024];
	if (read(m, rb, sizeof(rb)) <= 0)      { puts("FAIL drain"); return 0; }
	if (!((r = ev(s, POLLOUT)) & POLLOUT)) { printf("FAIL slave-pollout-after-drain rev=%#x\n", r); return 0; }

	close(s); close(m);
	puts("OK");
	return 0;
}
