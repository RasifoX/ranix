/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Edge-triggered epoll on a pipe gives an edge only for its own direction
 * (LTP epoll_wait06): a read is no new EPOLLIN edge, activity on another
 * pipe is none either, but a second write after EAGAIN is. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

int main(void)
{
	int p[2], q[2], bad = 0;
	static char buf[4096];
	if (pipe2(p, O_NONBLOCK) || pipe2(q, O_NONBLOCK) || fcntl(p[1], F_SETPIPE_SZ, 4096) != 4096) { puts("setup failed"); return 1; }
	int ep = epoll_create1(0);
	struct epoll_event a = { .events = EPOLLIN | EPOLLET, .data.fd = p[0] };
	struct epoll_event b = { .events = EPOLLOUT | EPOLLET, .data.fd = p[1] };
	epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &a);
	epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &b);
	struct epoll_event got[2];
	epoll_wait(ep, got, 2, 0);   /* the add-time edges */
	write(p[1], buf, 4096);
	int n = epoll_wait(ep, got, 2, 0);
	if (n != 1 || got[0].data.fd != p[0]) { printf("after the write: %d events, want EPOLLIN on the read end\n", n); bad++; }
	read(p[0], buf, 2048);
	n = epoll_wait(ep, got, 2, 0);
	if (n != 0) { printf("after reading half: %d events (fd %d, 0x%x), want none\n", n, got[0].data.fd, got[0].events); bad++; }
	write(q[1], "x", 1);   /* another pipe */
	n = epoll_wait(ep, got, 2, 0);
	if (n != 0) { printf("after a write to another pipe: %d events, want none\n", n); bad++; }
	read(p[0], buf, 2048);
	errno = 0;
	if (read(p[0], buf, 1) != -1 || errno != EAGAIN) { puts("pipe not empty"); bad++; }
	n = epoll_wait(ep, got, 2, 0);
	if (n != 1 || got[0].data.fd != p[1] || !(got[0].events & EPOLLOUT)) { printf("drained: %d events, want EPOLLOUT on the write end\n", n); bad++; }
	write(p[1], "y", 1);
	n = epoll_wait(ep, got, 2, 0);
	if (n != 1 || got[0].data.fd != p[0]) { printf("new data after EAGAIN: %d events, want an EPOLLIN edge\n", n); bad++; }
	write(p[1], "z", 1);   /* more data while still readable: a new edge on Linux too */
	n = epoll_wait(ep, got, 2, 0);
	if (n != 1 || got[0].data.fd != p[0]) { printf("a second write: %d events, want an EPOLLIN edge\n", n); bad++; }
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
