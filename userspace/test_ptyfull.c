/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A slave write into a full pty must wait for the master to read, never drop
 * bytes or return 0. Checked with output processing on (OPOST|ONLCR, every
 * '\n' becomes "\r\n") and off (raw). O_NONBLOCK on a full ring is EAGAIN, a
 * write after the master is gone is EIO. First failure or OK. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define FAIL(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); return 1; } while (0)
#define TOTAL (64 * 1024)

static int open_pair(int *m, int *s, int cooked)
{
	*m = posix_openpt(O_RDWR | O_NOCTTY);
	if (*m < 0 || grantpt(*m) || unlockpt(*m))
		return -1;
	*s = open(ptsname(*m), O_RDWR | O_NOCTTY);
	if (*s < 0)
		return -1;
	struct termios t;
	tcgetattr(*s, &t);
	cfmakeraw(&t);
	if (cooked)
		t.c_oflag |= OPOST | ONLCR;
	return tcsetattr(*s, TCSANOW, &t);
}

/* A child writes TOTAL bytes of numbered lines in 8 KiB writes; we start
 * reading only after it has filled the ring. */
static int run(int cooked)
{
	int m, s;
	if (open_pair(&m, &s, cooked))
		FAIL("pty open cooked=%d errno=%d", cooked, errno);
	pid_t pid = fork();
	if (pid == 0) {
		close(m);
		static char buf[TOTAL];
		for (int i = 0; i < TOTAL / 8; i++)
			snprintf(buf + i * 8, 9, "%07d\n", i);
		size_t off = 0;
		while (off < TOTAL) {
			size_t n = TOTAL - off < 8192 ? TOTAL - off : 8192;
			ssize_t w = write(s, buf + off, n);
			if (w <= 0)
				_exit(w == 0 ? 10 : 11);    /* 0 for a write of more than 0 */
			off += (size_t)w;
		}
		_exit(0);
	}
	close(s);
	usleep(300000);
	static char got[TOTAL * 2];
	size_t have = 0, want = cooked ? TOTAL + TOTAL / 8 : TOTAL;
	while (have < want) {
		ssize_t r = read(m, got + have, sizeof got - have);
		if (r <= 0)
			break;
		have += (size_t)r;
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
		FAIL("cooked=%d writer status=%d", cooked, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
	if (have != want)
		FAIL("cooked=%d read %zu of %zu bytes", cooked, have, want);
	size_t p = 0;
	for (int i = 0; i < TOTAL / 8; i++) {
		char line[16];
		snprintf(line, sizeof line, cooked ? "%07d\r\n" : "%07d\n", i);
		size_t l = strlen(line);
		if (memcmp(got + p, line, l))
			FAIL("cooked=%d line %d differs", cooked, i);
		p += l;
	}
	close(m);
	return 0;
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	if (run(1) || run(0))
		return 1;

	int m, s;
	if (open_pair(&m, &s, 1))
		FAIL("pty open errno=%d", errno);
	fcntl(s, F_SETFL, O_NONBLOCK);
	static char blk[8192];
	memset(blk, 'x', sizeof blk);
	size_t in = 0;
	ssize_t w;
	for (int i = 0; i < 64 && (w = write(s, blk, sizeof blk)) > 0; i++)
		in += (size_t)w;
	if (w != -1 || errno != EAGAIN)
		FAIL("nonblocking full write w=%zd errno=%d after %zu", w, errno, in);
	if (in == 0)
		FAIL("nonblocking write took nothing");
	close(m);
	fcntl(s, F_SETFL, 0);
	w = write(s, "a", 1);
	if (w != -1 || errno != EIO)
		FAIL("write after master close w=%zd errno=%d", w, errno);
	printf("cooked and raw %d bytes, full ring %zu\nOK\n", TOTAL, in);
	return 0;
}
