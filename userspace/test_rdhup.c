/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A stream socket whose read side is closed reports POLLRDHUP / EPOLLRDHUP
 * (LTP epoll_wait05): after its own shutdown(SHUT_RD), after the peer's FIN,
 * and for a unix socket whose peer shut down writing. A read then returns 0. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

static int bad;

static void tcp_pair(int *c, int *s)
{
	int l = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t len = sizeof(a);
	bind(l, (struct sockaddr *)&a, sizeof(a));
	listen(l, 1);
	getsockname(l, (struct sockaddr *)&a, &len);
	*c = socket(AF_INET, SOCK_STREAM, 0);
	connect(*c, (struct sockaddr *)&a, sizeof(a));
	*s = accept(l, NULL, NULL);
	close(l);
}

static void expect_rdhup(const char *what, int fd)
{
	int ep = epoll_create1(0);
	struct epoll_event ev = { .events = EPOLLRDHUP }, got = { 0 };
	epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
	int n = epoll_wait(ep, &got, 1, 2000);
	if (n != 1 || !(got.events & EPOLLRDHUP)) { printf("%s: epoll %d events 0x%x, want EPOLLRDHUP\n", what, n, got.events); bad++; }
	close(ep);
	struct pollfd p = { .fd = fd, .events = POLLIN | POLLRDHUP };
	if (poll(&p, 1, 2000) != 1 || !(p.revents & POLLRDHUP)) { printf("%s: poll revents 0x%x, want POLLRDHUP\n", what, p.revents); bad++; }
	char b;
	if (read(fd, &b, 1) != 0) { printf("%s: read did not return 0\n", what); bad++; }
}

int main(void)
{
	int c, s;
	tcp_pair(&c, &s);
	if (c < 0 || s < 0) { puts("no tcp loopback"); return 1; }
	shutdown(c, SHUT_RD);
	expect_rdhup("tcp after own SHUT_RD", c);
	close(c); close(s);

	tcp_pair(&c, &s);
	close(s);   /* the peer's FIN */
	expect_rdhup("tcp after the peer closed", c);
	close(c);

	int u[2];
	socketpair(AF_UNIX, SOCK_STREAM, 0, u);
	shutdown(u[1], SHUT_WR);
	expect_rdhup("unix after the peer's SHUT_WR", u[0]);
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
