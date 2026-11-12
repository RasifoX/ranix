/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A pty slave opened twice stays open until its last file is closed.
 *
 * sshd (dropbear, OpenSSH) makes the slave the controlling terminal and then
 * opens it once more by name and closes that fd again (pty_make_controlling_tty).
 * The shell keeps the first slave file as its stdin/stdout/stderr. The master
 * must not see a hangup for that close: no EOF on read while the shell runs, so
 * output written later (a nested "sh -c" after its fork + exec) still arrives.
 * When the last slave file does close, the master reads EOF (0 or EIO). */
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
	int s2 = open(path, O_RDWR | O_NOCTTY);           /* sshd's second open */
	if (s2 < 0) { puts("FAIL pts-open2"); return 0; }
	close(s2);

	/* the slave is still open through s: nothing to read, no hangup */
	struct pollfd pf = { m, POLLIN, 0 };
	int pr = poll(&pf, 1, 100);
	if (pr != 0) { printf("FAIL hup-after-reopen poll=%d rev=%#x\n", pr, pf.revents); return 0; }
	int fl = fcntl(m, F_GETFL);
	fcntl(m, F_SETFL, fl | O_NONBLOCK);
	char buf[16];
	ssize_t r = read(m, buf, sizeof(buf));
	if (!(r < 0 && errno == EAGAIN)) { printf("FAIL eof-after-reopen r=%zd errno=%d\n", r, r < 0 ? errno : 0); return 0; }
	fcntl(m, F_SETFL, fl);

	/* late output through the remaining slave file reaches the master */
	struct termios tio;
	if (tcgetattr(s, &tio) == 0) { cfmakeraw(&tio); tcsetattr(s, TCSANOW, &tio); }
	if (write(s, "late", 4) != 4) { puts("FAIL s-write"); return 0; }
	r = read(m, buf, sizeof(buf));
	if (r != 4 || memcmp(buf, "late", 4) != 0) { printf("FAIL late-read r=%zd\n", r); return 0; }

	/* input still flows the other way */
	if (write(m, "in", 2) != 2) { puts("FAIL m-write"); return 0; }
	r = read(s, buf, sizeof(buf));
	if (r != 2 || memcmp(buf, "in", 2) != 0) { printf("FAIL s-read r=%zd\n", r); return 0; }

	/* the last slave close is the hangup: EOF on the master */
	close(s);
	pf.revents = 0;
	pr = poll(&pf, 1, 1000);
	if (pr != 1) { printf("FAIL no-hup-after-last-close poll=%d\n", pr); return 0; }
	r = read(m, buf, sizeof(buf));
	if (!(r == 0 || (r < 0 && errno == EIO))) { printf("FAIL eof-after-last-close r=%zd errno=%d\n", r, r < 0 ? errno : 0); return 0; }

	close(m);
	puts("OK");
	return 0;
}
