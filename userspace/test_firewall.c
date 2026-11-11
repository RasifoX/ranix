/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The packet filter as a wall with doors (kernel/net/netfilter.c), over
 * loopback without the "lo" door, so loopback packets meet the wall like any
 * other: "policy in drop" drops what no rule lets in; a door (accept tcp dport
 * N) lets a connection in; "established" lets the answers to what this machine
 * started back in (TCP by socket, UDP by flow); without it a connect out stalls;
 * "lo" opens the loopback; ICMP types parse; "flush" is the boot state again.
 * Root. First failure or OK. Leaves the filter flushed on every path. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SOL_RANIX_FILTER 0x52414e58
#define NF_RULE 1

#define P_DOOR   39101
#define P_SHUT   39102
#define U_DOOR   39103
#define U_SHUT   39104

static int fw = -1;

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s errno=%d\n", __LINE__, #c, errno); \
	if (fw >= 0) { setsockopt(fw, SOL_RANIX_FILTER, NF_RULE, "flush", 5); } exit(1); } } while (0)

static int cmd(const char *s)
{
	return setsockopt(fw, SOL_RANIX_FILTER, NF_RULE, s, (socklen_t)strlen(s));
}

static void must(const char *s)
{
	if (cmd(s) != 0) {
		printf("FAIL rule '%s' errno=%d\n", s, errno);
		cmd("flush");
		exit(1);
	}
}

static void dump(char *b, size_t cap)
{
	socklen_t l = (socklen_t)cap - 1;
	memset(b, 0, cap);
	CHECK(getsockopt(fw, SOL_RANIX_FILTER, NF_RULE, b, &l) == 0);
}

static struct sockaddr_in lo_addr(int port)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	return a;
}

static int listener(int port)
{
	int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	struct sockaddr_in a = lo_addr(port);
	CHECK(s >= 0);
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	CHECK(bind(s, (struct sockaddr *)&a, sizeof(a)) == 0);
	CHECK(listen(s, 4) == 0);
	return s;
}

/* 1 when a connect to port completes within ms, 0 when it is still waiting */
static int connects(int port, int ms, int *out)
{
	int c = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = lo_addr(port);
	CHECK(c >= 0);
	fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
	int r = connect(c, (struct sockaddr *)&a, sizeof(a));
	CHECK(r == 0 || errno == EINPROGRESS);
	struct pollfd p = { .fd = c, .events = POLLOUT };
	int n = r == 0 ? 1 : poll(&p, 1, ms);
	int err = 0;
	socklen_t el = sizeof(err);
	if (n == 1) getsockopt(c, SOL_SOCKET, SO_ERROR, &err, &el);
	if (out && n == 1 && !err) { *out = c; return 1; }
	close(c);
	return n == 1 && !err;
}

static int readable(int s, int ms)
{
	struct pollfd p = { .fd = s, .events = POLLIN };
	return poll(&p, 1, ms) == 1;
}

static int udp_bound(int port)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a = lo_addr(port);
	CHECK(s >= 0);
	CHECK(bind(s, (struct sockaddr *)&a, sizeof(a)) == 0);
	return s;
}

int main(void)
{
	char b[4096];
	fw = socket(AF_INET, SOCK_DGRAM, 0);
	CHECK(fw >= 0);
	CHECK(cmd("flush") == 0);
	dump(b, sizeof(b));
	CHECK(strstr(b, "policy in accept") && strstr(b, "policy out accept"));

	/* grammar: types are ICMP's, ranges, bad words */
	CHECK(cmd("accept tcp type 8") != 0 && errno == EINVAL);
	CHECK(cmd("policy in maybe") != 0 && errno == EINVAL);
	CHECK(cmd("policy sideways drop") != 0 && errno == EINVAL);
	CHECK(cmd("accept icmp type 300") != 0 && errno == EINVAL);
	CHECK(cmd("accept icmpv6 type 137-133") != 0 && errno == EINVAL);
	must("accept icmpv6 type 133-137");
	must("accept icmp type 8");
	must("accept any lo");
	must("accept any established");
	dump(b, sizeof(b));
	CHECK(strstr(b, "in accept icmpv6 type 133-137\n") && strstr(b, "in accept icmp type 8\n"));
	CHECK(strstr(b, "in accept any lo\n") && strstr(b, "in accept any established\n"));
	must("flush");

	int ld = listener(P_DOOR), ls = listener(P_SHUT);

	/* 1. the wall with one door and established */
	must("policy in drop");
	must("accept any established");
	{ char r[48]; snprintf(r, sizeof(r), "accept tcp dport %d", P_DOOR); must(r); }
	dump(b, sizeof(b));
	CHECK(strstr(b, "policy in drop"));
	int c = -1;
	CHECK(connects(P_DOOR, 2000, &c) == 1);            /* the door */
	int a = accept(ld, NULL, NULL);
	CHECK(a >= 0);
	CHECK(write(c, "x", 1) == 1 && readable(a, 1000));
	CHECK(write(a, "y", 1) == 1 && readable(c, 1000));  /* answers come back */
	close(a); close(c);
	CHECK(connects(P_SHUT, 1500, NULL) == 0);          /* no door: dropped, no RST */
	dump(b, sizeof(b));
	CHECK(!strstr(b, "dropped in 0 "));

	/* UDP: the door, the flow back, the shut port */
	{ char r[48]; snprintf(r, sizeof(r), "accept udp dport %d", U_DOOR); must(r); }
	int us = udp_bound(U_DOOR), ux = udp_bound(U_SHUT), uc = socket(AF_INET, SOCK_DGRAM, 0);
	CHECK(uc >= 0);
	struct sockaddr_in ua = lo_addr(U_DOOR), xa = lo_addr(U_SHUT), from;
	socklen_t fl = sizeof(from);
	CHECK(sendto(uc, "q", 1, 0, (struct sockaddr *)&ua, sizeof(ua)) == 1);
	CHECK(readable(us, 1000));
	char q;
	CHECK(recvfrom(us, &q, 1, 0, (struct sockaddr *)&from, &fl) == 1);
	CHECK(sendto(us, "r", 1, 0, (struct sockaddr *)&from, fl) == 1);
	CHECK(readable(uc, 1000));                          /* the flow uc started */
	CHECK(recv(uc, &q, 1, 0) == 1 && q == 'r');
	CHECK(sendto(uc, "s", 1, 0, (struct sockaddr *)&xa, sizeof(xa)) == 1);
	CHECK(!readable(ux, 700));                          /* no door */
	close(us); close(ux); close(uc);

	/* 2. without established, a connection this machine starts stalls: the
	 *    door lets the SYN in, the SYN-ACK back is dropped */
	must("flush");
	must("policy in drop");
	{ char r[48]; snprintf(r, sizeof(r), "accept tcp dport %d", P_DOOR); must(r); }
	CHECK(connects(P_DOOR, 1500, NULL) == 0);
	/* the half-open one in the listener's queue is TCP's to clean up */

	/* 3. the loopback door */
	must("flush");
	must("policy in drop");
	must("accept any lo");
	c = -1;
	CHECK(connects(P_SHUT, 2000, &c) == 1);
	close(c);

	/* 4. flush is the boot state: everything in */
	must("flush");
	dump(b, sizeof(b));
	CHECK(strstr(b, "policy in accept"));
	c = -1;
	CHECK(connects(P_SHUT, 2000, &c) == 1);
	close(c);
	close(ld); close(ls);
	puts("OK");
	return 0;
}
