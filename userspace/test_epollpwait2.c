/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* epoll_pwait2 (441): timespec timeout, NULL waits, bad nsec is EINVAL, a sigmask
 * is accepted. libuv in Claude Code calls it first. First failure or OK. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static long pw2(int ep, struct epoll_event *ev, int n, const struct timespec *ts, const sigset_t *m)
{
	return syscall(441, ep, ev, n, ts, m, m ? 8 : 0);
}

static long ms_now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(void)
{
	int p[2], ep = epoll_create1(0);
	struct epoll_event e = { .events = EPOLLIN }, out[4];
	if (ep < 0 || pipe(p)) { puts("FAIL setup"); return 1; }
	e.data.fd = p[0];
	if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &e)) { puts("FAIL ctl"); return 1; }

	struct timespec zero = { 0, 0 }, t60 = { 0, 60000000 }, bad = { 0, 1000000000 };
	if (pw2(ep, out, 4, &zero, NULL) != 0) { puts("FAIL zero"); return 1; }
	long t0 = ms_now(), r = pw2(ep, out, 4, &t60, NULL), dt = ms_now() - t0;
	if (r != 0 || dt < 40 || dt > 2000) { printf("FAIL timeout r=%ld dt=%ld\n", r, dt); return 1; }
	errno = 0;
	if (pw2(ep, out, 4, &bad, NULL) != -1 || errno != EINVAL) { printf("FAIL badnsec errno=%d\n", errno); return 1; }
	if (write(p[1], "x", 1) != 1) { puts("FAIL write"); return 1; }
	sigset_t m;
	sigemptyset(&m);
	sigaddset(&m, SIGUSR1);
	r = pw2(ep, out, 4, NULL, &m);
	if (r != 1 || out[0].data.fd != p[0] || !(out[0].events & EPOLLIN)) { printf("FAIL ready r=%ld\n", r); return 1; }
	errno = 0;
	if (pw2(-1, out, 4, &zero, NULL) != -1 || errno != EBADF) { printf("FAIL ebadf errno=%d\n", errno); return 1; }
	puts("OK");
	return 0;
}
