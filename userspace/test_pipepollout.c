/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A pipe is writable for poll only with a page free, as on Linux (LTP
 * epoll_wait06): a full one-page pipe read half way is still not writable,
 * and an edge-triggered EPOLLOUT comes once it is drained. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

int main(void)
{
	int p[2], bad = 0;
	static char buf[4096];
	if (pipe2(p, O_NONBLOCK) || fcntl(p[1], F_SETPIPE_SZ, 4096) != 4096) { puts("setup failed"); return 1; }
	int ep = epoll_create1(0);
	struct epoll_event ev = { .events = EPOLLOUT | EPOLLET, .data.fd = p[1] }, got;
	epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &ev);
	if (write(p[1], buf, 4096) != 4096) { puts("fill failed"); return 1; }
	epoll_wait(ep, &got, 1, 0);   /* the add-time edge, if any */
	if (read(p[0], buf, 2048) != 2048) { puts("read failed"); return 1; }
	struct pollfd pf = { .fd = p[1], .events = POLLOUT };
	if (poll(&pf, 1, 0) != 0) { printf("half-read one-page pipe: poll says POLLOUT\n"); bad++; }
	if (epoll_wait(ep, &got, 1, 0) != 0) { printf("half-read one-page pipe: EPOLLOUT edge\n"); bad++; }
	read(p[0], buf, 2048);
	if (epoll_wait(ep, &got, 1, 0) != 1 || !(got.events & EPOLLOUT)) { printf("drained pipe: no EPOLLOUT edge\n"); bad++; }
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
