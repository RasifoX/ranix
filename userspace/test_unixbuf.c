/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* An AF_UNIX stream's unread data is bounded by the send buffer, as on Linux.
 * Ranix had no bound: a writer whose reader never read queued kernel memory
 * until the OOM killer took bystanders (found by test_fuzz, 1 GB in socket
 * buffers). A nonblocking writer must get EAGAIN after a few hundred KB, also
 * with one-byte writes; poll must then say "not writable" and say "writable"
 * again once the reader drained it; and a blocking writer of 4 MB must finish
 * promptly against a reader, woken when room comes, not by a timeout. Prints
 * OK or FAIL lines. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } while (0)

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* bytes a nonblocking writer gets in before EAGAIN, chunk bytes per write;
 * stops at cap (no bound) */
static long fill(int fd, size_t chunk, long cap)
{
	static char b[4096];
	long total = 0;
	while (total < cap) {
		ssize_t r = write(fd, b, chunk);
		if (r < 0) return errno == EAGAIN ? total : -1;
		total += r;
	}
	return total;
}

static void drain(int fd)
{
	char b[4096];
	while (read(fd, b, sizeof(b)) > 0) {}
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	const long cap = 64L << 20;
	for (int one = 0; one < 2; one++) {
		int sv[2];
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { FAIL("socketpair: %s", strerror(errno)); return 1; }
		fcntl(sv[0], F_SETFL, O_NONBLOCK);
		fcntl(sv[1], F_SETFL, O_NONBLOCK);
		long got = fill(sv[0], one ? 1 : 4096, one ? 100000L : cap);
		const char *how = one ? "one-byte writes" : "4 KB writes";
		if (got < 0) FAIL("%s: write: %s", how, strerror(errno));
		else if (got >= (one ? 100000L : cap)) FAIL("%s: no EAGAIN after %ld bytes unread", how, got);
		else if (!one && got < 65536) FAIL("%s: EAGAIN after only %ld bytes", how, got);
		struct pollfd p = { sv[0], POLLOUT, 0 };
		if (poll(&p, 1, 0) != 0) FAIL("%s: full socket polls writable (revents 0x%x)", how, p.revents);
		drain(sv[1]);
		p.revents = 0;
		if (poll(&p, 1, 1000) != 1 || !(p.revents & POLLOUT)) FAIL("%s: drained socket not writable", how);
		close(sv[0]);
		close(sv[1]);
	}

	/* a blocking writer against a reader: woken for room, not by a timeout */
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { FAIL("socketpair: %s", strerror(errno)); return 1; }
	const long big = 4L << 20;
	double t0 = now();
	pid_t c = fork();
	if (c == 0) {
		static char b[65536];
		long left = big;
		while (left > 0) {
			ssize_t r = write(sv[0], b, left < (long)sizeof(b) ? (size_t)left : sizeof(b));
			if (r <= 0) _exit(1);
			left -= r;
		}
		_exit(0);
	}
	close(sv[0]);
	long total = 0;
	char b[8192];
	ssize_t r;
	while (total < big && now() - t0 < 10 && (r = read(sv[1], b, sizeof(b))) > 0) total += r;
	double took = now() - t0;
	int st = 0;
	if (total < big) { kill(c, SIGKILL); FAIL("blocking 4 MB: read %ld bytes in %.1f s", total, took); }
	waitpid(c, &st, 0);
	if (total == big && (!WIFEXITED(st) || WEXITSTATUS(st))) FAIL("blocking 4 MB: the writer failed");
	if (total == big && took > 3) FAIL("blocking 4 MB took %.1f s (woken by timeouts?)", took);
	printf("blocking 4 MB in %.2f s\n", took);
	close(sv[1]);
	printf(fails ? "FAILED %d\n" : "OK\n", fails);
	return fails != 0;
}
