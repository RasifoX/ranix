/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* NETLINK_ROUTE from the raw ABI up: framing, port ids, dumps, single gets,
 * errors, namespace scoping, and libc's getifaddrs on top. Prints one line per
 * case; the last line is OK or the number of failures. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>

#define BUF 8192

static unsigned seqno = 100;

static int nl_open(int type)
{
	int fd = socket(AF_NETLINK, type | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0) return -1;
	struct sockaddr_nl a = { .nl_family = AF_NETLINK };
	if (bind(fd, (void *)&a, sizeof(a)) != 0) { close(fd); return -1; }
	return fd;
}

static unsigned port_of(int fd)
{
	struct sockaddr_nl a;
	socklen_t l = sizeof(a);
	if (getsockname(fd, (void *)&a, &l) != 0) return 0;
	return a.nl_pid;
}

static int ask(int fd, int type, int flags, int family, unsigned *seq)
{
	struct { struct nlmsghdr h; struct rtgenmsg g; } r;
	memset(&r, 0, sizeof(r));
	r.h.nlmsg_len   = NLMSG_LENGTH(sizeof(r.g));
	r.h.nlmsg_type  = (unsigned short)type;
	r.h.nlmsg_flags = (unsigned short)(NLM_F_REQUEST | flags);
	r.h.nlmsg_seq   = *seq = ++seqno;
	r.g.rtgen_family = (unsigned char)family;
	struct sockaddr_nl to = { .nl_family = AF_NETLINK };
	return sendto(fd, &r, r.h.nlmsg_len, 0, (void *)&to, sizeof(to)) == (long)r.h.nlmsg_len ? 0 : -1;
}

typedef int (*msg_cb)(struct nlmsghdr *h, void *arg);

/* walks a dump strictly: every part well framed, seq/pid echoed, NLM_F_MULTI
 * set, and exactly one NLMSG_DONE closing it; nonblocking, so a missing DONE
 * fails instead of hanging */
static int walk_dump(int fd, unsigned seq, msg_cb cb, void *arg, int *parts)
{
	static char b[BUF];
	unsigned me = port_of(fd);
	int n = 0;
	for (;;) {
		struct sockaddr_nl from;
		struct iovec v = { b, sizeof(b) };
		struct msghdr m = { .msg_name = &from, .msg_namelen = sizeof(from),
		                    .msg_iov = &v, .msg_iovlen = 1 };
		long len = recvmsg(fd, &m, MSG_DONTWAIT);
		if (len <= 0) return 1;
		if (m.msg_namelen != sizeof(from) || from.nl_family != AF_NETLINK || from.nl_pid != 0)
			return 2;
		if (m.msg_flags & MSG_TRUNC) return 3;
		struct nlmsghdr *h = (void *)b;
		for (; NLMSG_OK(h, (unsigned long)len); h = NLMSG_NEXT(h, len)) {
			if (h->nlmsg_seq != seq || h->nlmsg_pid != me) return 4;
			if (!(h->nlmsg_flags & NLM_F_MULTI)) return 5;
			if (h->nlmsg_type == NLMSG_ERROR) return 6;
			if (h->nlmsg_type == NLMSG_DONE) {
				if (h->nlmsg_len != NLMSG_LENGTH(sizeof(int))) return 7;
				if (recv(fd, b, sizeof(b), MSG_DONTWAIT) != -1 || errno != EAGAIN)
					return 8;                  /* nothing may follow DONE */
				if (parts) *parts = n;
				return 0;
			}
			n++;
			if (cb && cb(h, arg)) return 9;
		}
		if (len != 0) return 10;                   /* no remnant */
	}
}

/* error code of the NLMSG_ERROR answering seq; 1 if a different type came */
static int read_ack(int fd, unsigned seq, int *got_type)
{
	char b[1024];
	long n = recv(fd, b, sizeof(b), MSG_DONTWAIT);
	if (n <= 0) return 1000;
	struct nlmsghdr *h = (void *)b;
	if (!NLMSG_OK(h, (unsigned long)n) || h->nlmsg_seq != seq) return 1001;
	if (got_type) *got_type = h->nlmsg_type;
	if (h->nlmsg_type != NLMSG_ERROR) return 1;
	struct nlmsgerr *e = NLMSG_DATA(h);
	if (h->nlmsg_len < NLMSG_LENGTH(sizeof(*e))) return 1002;
	if (e->msg.nlmsg_seq != seq) return 1003;
	return e->error;
}

static struct rtattr *attr(void *body, long len, int type)
{
	struct rtattr *a = body;
	for (; RTA_OK(a, len); a = RTA_NEXT(a, len))
		if (a->rta_type == type) return a;
	return 0;
}

/* ── collected dump state ─────────────────────────────────────────────── */

static struct { int lo_index, eth_index, count, lo_flags; char eth_mac[6]; char eth_name[16]; } L;

static int on_link(struct nlmsghdr *h, void *arg)
{
	(void)arg;
	if (h->nlmsg_type != RTM_NEWLINK) return 1;
	struct ifinfomsg *i = NLMSG_DATA(h);
	long alen = (long)h->nlmsg_len - NLMSG_LENGTH(sizeof(*i));
	struct rtattr *n = attr(IFLA_RTA(i), alen, IFLA_IFNAME);
	if (!n) return 1;
	L.count++;
	if (strcmp(RTA_DATA(n), "lo") == 0) {
		L.lo_index = i->ifi_index;
		L.lo_flags = (int)i->ifi_flags;
		if (i->ifi_type != 772) return 1;
	} else {
		L.eth_index = i->ifi_index;
		strncpy(L.eth_name, RTA_DATA(n), 15);
		struct rtattr *m = attr(IFLA_RTA(i), alen, IFLA_ADDRESS);
		if (!m || RTA_PAYLOAD(m) != 6) return 1;
		memcpy(L.eth_mac, RTA_DATA(m), 6);
	}
	return attr(IFLA_RTA(i), alen, IFLA_MTU) ? 0 : 1;
}

static int read_links(int fd)
{
	unsigned seq;
	memset(&L, 0, sizeof(L));
	if (ask(fd, RTM_GETLINK, NLM_F_DUMP, AF_UNSPEC, &seq)) return 100;
	return walk_dump(fd, seq, on_link, 0, 0);
}

static struct { int v4, v6, lo4, lo_prefix; } A;

static int on_addr(struct nlmsghdr *h, void *arg)
{
	(void)arg;
	if (h->nlmsg_type != RTM_NEWADDR) return 1;
	struct ifaddrmsg *a = NLMSG_DATA(h);
	long alen = (long)h->nlmsg_len - NLMSG_LENGTH(sizeof(*a));
	struct rtattr *r = attr(IFA_RTA(a), alen, IFA_ADDRESS);
	if (!r) return 1;
	if (a->ifa_family == AF_INET) {
		A.v4++;
		if (*(unsigned *)RTA_DATA(r) == htonl(INADDR_LOOPBACK)) {
			A.lo4 = 1;
			A.lo_prefix = a->ifa_prefixlen;
			if (!attr(IFA_RTA(a), alen, IFA_LOCAL)) return 1;
			if ((int)a->ifa_index != L.lo_index) return 1;
		}
	} else if (a->ifa_family == AF_INET6) {
		A.v6++;
	} else {
		return 1;
	}
	return 0;
}

static struct { int main_routes, default_routes, gw; } R;

static int on_route(struct nlmsghdr *h, void *arg)
{
	(void)arg;
	if (h->nlmsg_type != RTM_NEWROUTE) return 1;
	struct rtmsg *r = NLMSG_DATA(h);
	long alen = (long)h->nlmsg_len - NLMSG_LENGTH(sizeof(*r));
	if (!attr(RTM_RTA(r), alen, RTA_OIF)) return 1;
	if (r->rtm_table == RT_TABLE_MAIN) R.main_routes++;
	if (r->rtm_dst_len == 0 && r->rtm_family == AF_INET) {
		R.default_routes++;
		if (attr(RTM_RTA(r), alen, RTA_GATEWAY)) R.gw = 1;
	}
	return 0;
}

/* ── cases ────────────────────────────────────────────────────────────── */

static int t_socket(void)
{
	if (socket(AF_NETLINK, SOCK_STREAM, NETLINK_ROUTE) != -1 || errno != ESOCKTNOSUPPORT) return 1;
	if (socket(AF_NETLINK, SOCK_RAW, 99) != -1 || errno != EPROTONOSUPPORT) return 2;
	int a = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	int b = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
	if (a < 0 || b < 0) return 3;
	return 0;
}

static int t_bind_names(void)
{
	int a = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	struct sockaddr_nl n;
	socklen_t l = sizeof(n);
	memset(&n, 0xee, sizeof(n));
	if (getsockname(a, (void *)&n, &l) != 0 || l != sizeof(n) ||
	    n.nl_family != AF_NETLINK || n.nl_pid != 0) return 1;

	struct sockaddr_nl s = { .nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK };
	if (bind(a, (void *)&s, 4) != -1 || errno != EINVAL) return 2;
	struct sockaddr_in { unsigned short f; char x[14]; } in = { AF_INET, {0} };
	if (bind(a, (void *)&in, sizeof(in)) != -1 || errno != EINVAL) return 3;
	if (bind(a, (void *)&s, sizeof(s)) != 0) return 4;
	l = sizeof(n);
	if (getsockname(a, (void *)&n, &l) != 0 || n.nl_pid == 0 ||
	    n.nl_groups != RTMGRP_LINK) return 5;

	int b = nl_open(SOCK_RAW);
	if (b < 0 || port_of(b) == n.nl_pid) return 6;   /* same process, two ports */

	struct sockaddr_nl dup = { .nl_family = AF_NETLINK, .nl_pid = n.nl_pid };
	int c = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (bind(c, (void *)&dup, sizeof(dup)) != -1 || errno != EADDRINUSE) return 7;
	dup.nl_pid = 0x12345;
	if (bind(c, (void *)&dup, sizeof(dup)) != 0 || port_of(c) != 0x12345) return 8;

	l = sizeof(n);
	if (getpeername(a, (void *)&n, &l) != 0 || n.nl_pid != 0) return 9;

	/* short name buffer: truncated copy, real length reported */
	char tiny[6];
	l = sizeof(tiny);
	if (getsockname(a, (void *)tiny, &l) != 0 || l != sizeof(struct sockaddr_nl)) return 10;
	return 0;
}

static int t_autobind_on_send(void)
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	unsigned seq;
	if (ask(fd, RTM_GETLINK, NLM_F_DUMP, AF_UNSPEC, &seq)) return 1;
	if (port_of(fd) == 0) return 2;
	return walk_dump(fd, seq, 0, 0, 0) ? 3 : 0;
}

static int t_link_dump(void)
{
	int fd = nl_open(SOCK_RAW);
	int r = read_links(fd);
	if (r) return r;
	if (!L.lo_index) return 20;
	if (!(L.lo_flags & IFF_LOOPBACK) || !(L.lo_flags & IFF_UP)) return 21;

	/* the index must be the one the ioctl path hands out */
	struct ifreq q;
	memset(&q, 0, sizeof(q));
	strcpy(q.ifr_name, "lo");
	if (ioctl(fd, SIOCGIFINDEX, &q) != 0 || q.ifr_ifindex != L.lo_index) return 22;
	if (L.eth_index) {
		strncpy(q.ifr_name, L.eth_name, IFNAMSIZ - 1);
		if (ioctl(fd, SIOCGIFINDEX, &q) != 0 || q.ifr_ifindex != L.eth_index) return 23;
		if (ioctl(fd, SIOCGIFHWADDR, &q) != 0) return 24;
		if (memcmp(q.ifr_hwaddr.sa_data, L.eth_mac, 6) != 0) return 25;
	}
	return 0;
}

static int t_addr_dump(void)
{
	int fd = nl_open(SOCK_RAW);
	read_links(fd);
	unsigned seq;
	memset(&A, 0, sizeof(A));
	if (ask(fd, RTM_GETADDR, NLM_F_DUMP, AF_UNSPEC, &seq)) return 1;
	int r = walk_dump(fd, seq, on_addr, 0, 0);
	if (r) return 10 + r;
	if (!A.lo4 || A.lo_prefix != 8 || !A.v6) return 2;

	memset(&A, 0, sizeof(A));
	if (ask(fd, RTM_GETADDR, NLM_F_DUMP, AF_INET, &seq)) return 3;
	if (walk_dump(fd, seq, on_addr, 0, 0)) return 4;
	if (!A.v4 || A.v6) return 5;

	memset(&A, 0, sizeof(A));
	if (ask(fd, RTM_GETADDR, NLM_F_DUMP, AF_INET6, &seq)) return 6;
	if (walk_dump(fd, seq, on_addr, 0, 0)) return 7;
	if (A.v4 || !A.v6) return 8;
	return 0;
}

static int t_route_dump(void)
{
	int fd = nl_open(SOCK_RAW);
	unsigned seq;
	memset(&R, 0, sizeof(R));
	if (ask(fd, RTM_GETROUTE, NLM_F_DUMP, AF_INET, &seq)) return 1;
	int r = walk_dump(fd, seq, on_route, 0, 0);
	if (r) return 10 + r;
	if (R.default_routes && !R.gw) return 2;   /* a default must name its gateway */
	return 0;
}

/* two iovecs make one datagram (iproute2's rtnl_dump_request does exactly this) */
static int t_sendmsg_iovecs(void)
{
	int fd = nl_open(SOCK_RAW);
	struct nlmsghdr h = { .nlmsg_len = NLMSG_LENGTH(sizeof(struct rtgenmsg)),
	                      .nlmsg_type = RTM_GETLINK,
	                      .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP, .nlmsg_seq = 77 };
	struct rtgenmsg g = { AF_UNSPEC };
	char pad[3] = { 0, 0, 0 };
	struct iovec v[3] = { { &h, sizeof(h) }, { &g, sizeof(g) }, { pad, 3 } };
	struct sockaddr_nl to = { .nl_family = AF_NETLINK };
	struct msghdr m = { .msg_name = &to, .msg_namelen = sizeof(to), .msg_iov = v, .msg_iovlen = 3 };
	if (sendmsg(fd, &m, 0) != (long)(sizeof(h) + sizeof(g) + sizeof(pad))) return 1;
	int parts = 0;
	if (walk_dump(fd, 77, 0, 0, &parts)) return 2;
	return parts ? 0 : 3;
}

static int t_write_read(void)
{
	int fd = nl_open(SOCK_DGRAM);
	struct { struct nlmsghdr h; struct rtgenmsg g; } r;
	memset(&r, 0, sizeof(r));
	r.h.nlmsg_len = NLMSG_LENGTH(sizeof(r.g));
	r.h.nlmsg_type = RTM_GETADDR;
	r.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	r.h.nlmsg_seq = 5;
	if (write(fd, &r, r.h.nlmsg_len) != (long)r.h.nlmsg_len) return 1;
	char b[BUF];
	long n = read(fd, b, sizeof(b));
	if (n <= 0 || !NLMSG_OK((struct nlmsghdr *)b, (unsigned long)n)) return 2;
	return 0;
}

static int t_truncate_peek(void)
{
	int fd = nl_open(SOCK_RAW);
	unsigned seq;
	if (ask(fd, RTM_GETLINK, NLM_F_DUMP, AF_UNSPEC, &seq)) return 1;

	struct pollfd p = { fd, POLLIN, 0 };
	if (poll(&p, 1, 0) != 1) return 2;

	/* size the datagram without consuming it */
	char tiny[8];
	struct iovec v = { 0, 0 };
	struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1 };
	long want = recvmsg(fd, &m, MSG_PEEK | MSG_TRUNC);
	if (want <= (long)sizeof(tiny)) return 3;
	struct msghdr none = { 0 };
	if (recvmsg(fd, &none, MSG_PEEK | MSG_TRUNC) != want) return 4;

	/* a short read reports MSG_TRUNC and drops the rest of that datagram */
	v.iov_base = tiny; v.iov_len = sizeof(tiny);
	memset(&m, 0, sizeof(m));
	m.msg_iov = &v; m.msg_iovlen = 1;
	if (recvmsg(fd, &m, MSG_PEEK) != (long)sizeof(tiny) || !(m.msg_flags & MSG_TRUNC)) return 5;
	if (recvmsg(fd, &m, 0) != (long)sizeof(tiny) || !(m.msg_flags & MSG_TRUNC)) return 6;
	long left = recv(fd, tiny, 0, MSG_PEEK | MSG_TRUNC | MSG_DONTWAIT);
	if (left == want) return 7;
	return 0;
}

static int t_nonblocking(void)
{
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK, NETLINK_ROUTE);
	char b[64];
	if (recv(fd, b, sizeof(b), 0) != -1 || errno != EAGAIN) return 1;
	struct iovec v = { b, sizeof(b) };
	struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1 };
	if (recvmsg(fd, &m, 0) != -1 || errno != EAGAIN) return 2;
	if (read(fd, b, sizeof(b)) != -1 || errno != EAGAIN) return 3;
	return 0;
}

static int t_single_get(void)
{
	int fd = nl_open(SOCK_RAW);
	read_links(fd);

	struct { struct nlmsghdr h; struct ifinfomsg i; } r;
	memset(&r, 0, sizeof(r));
	r.h.nlmsg_len = NLMSG_LENGTH(sizeof(r.i));
	r.h.nlmsg_type = RTM_GETLINK;
	r.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	r.h.nlmsg_seq = 9;
	r.i.ifi_index = L.lo_index;
	if (send(fd, &r, r.h.nlmsg_len, 0) != (long)r.h.nlmsg_len) return 1;
	int type = 0;
	if (read_ack(fd, 9, &type) != 1 || type != RTM_NEWLINK) return 2;
	if (read_ack(fd, 9, 0) != 0) return 3;                /* then the ack */

	r.i.ifi_index = 999;
	r.h.nlmsg_seq = 10;
	if (send(fd, &r, r.h.nlmsg_len, 0) != (long)r.h.nlmsg_len) return 4;
	if (read_ack(fd, 10, 0) != -ENODEV) return 5;

	/* by name */
	struct { struct nlmsghdr h; struct ifinfomsg i; struct rtattr a; char n[4]; } q;
	memset(&q, 0, sizeof(q));
	q.h.nlmsg_len = sizeof(q);
	q.h.nlmsg_type = RTM_GETLINK;
	q.h.nlmsg_flags = NLM_F_REQUEST;
	q.h.nlmsg_seq = 11;
	q.a.rta_type = IFLA_IFNAME;
	q.a.rta_len = RTA_LENGTH(3);
	memcpy(q.n, "lo", 3);
	if (send(fd, &q, sizeof(q), 0) != (long)sizeof(q)) return 6;
	if (read_ack(fd, 11, &type) != 1 || type != RTM_NEWLINK) return 7;
	return 0;
}

static int route_get(int fd, unsigned dst_be, int *oif, unsigned char *rtype)
{
	struct { struct nlmsghdr h; struct rtmsg r; struct rtattr a; unsigned d; } q;
	memset(&q, 0, sizeof(q));
	q.h.nlmsg_len = sizeof(q);
	q.h.nlmsg_type = RTM_GETROUTE;
	q.h.nlmsg_flags = NLM_F_REQUEST;
	q.h.nlmsg_seq = ++seqno;
	q.r.rtm_family = AF_INET;
	q.a.rta_type = RTA_DST;
	q.a.rta_len = RTA_LENGTH(4);
	q.d = dst_be;
	if (send(fd, &q, sizeof(q), 0) != (long)sizeof(q)) return -1;
	char b[1024];
	long n = recv(fd, b, sizeof(b), MSG_DONTWAIT);
	struct nlmsghdr *h = (void *)b;
	if (n <= 0 || !NLMSG_OK(h, (unsigned long)n)) return -2;
	if (h->nlmsg_type == NLMSG_ERROR) return ((struct nlmsgerr *)NLMSG_DATA(h))->error;
	if (h->nlmsg_type != RTM_NEWROUTE) return -3;
	struct rtmsg *r = NLMSG_DATA(h);
	struct rtattr *o = attr(RTM_RTA(r), (long)h->nlmsg_len - NLMSG_LENGTH(sizeof(*r)), RTA_OIF);
	if (!o) return -4;
	*oif = *(int *)RTA_DATA(o);
	*rtype = r->rtm_type;
	return 0;
}

static int t_route_get(void)
{
	int fd = nl_open(SOCK_RAW);
	read_links(fd);
	int oif = 0;
	unsigned char t = 0;
	int r = route_get(fd, htonl(INADDR_LOOPBACK), &oif, &t);
	if (r) { printf("  route_get=%d\n", r); return 1; }
	if (oif != L.lo_index || t != RTN_LOCAL) return 2;
	return 0;
}

static int mutate_err(int fd, int type)
{
	struct { struct nlmsghdr h; char body[32]; } r;
	memset(&r, 0, sizeof(r));
	r.h.nlmsg_len = NLMSG_LENGTH(sizeof(r.body));
	r.h.nlmsg_type = (unsigned short)type;
	r.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE;
	r.h.nlmsg_seq = ++seqno;
	if (send(fd, &r, r.h.nlmsg_len, 0) != (long)r.h.nlmsg_len) return 1;
	return read_ack(fd, r.h.nlmsg_seq, 0);
}

static const int mutators[] = { RTM_NEWLINK, RTM_DELLINK, RTM_SETLINK, RTM_NEWADDR, RTM_DELADDR,
                                RTM_NEWROUTE, RTM_DELROUTE };

static int t_mutate(void)
{
	int fd = nl_open(SOCK_RAW);
	for (unsigned i = 0; i < sizeof(mutators) / sizeof(mutators[0]); i++)
		if (mutate_err(fd, mutators[i]) != -EOPNOTSUPP) return 1;

	if (mutate_err(fd, RTM_GETNEIGH + 100) != -EOPNOTSUPP) return 2;   /* unknown get */

	pid_t pid = fork();
	if (pid == 0) {
		if (setuid(1000) != 0) _exit(1);
		for (unsigned i = 0; i < sizeof(mutators) / sizeof(mutators[0]); i++)
			if (mutate_err(fd, mutators[i]) != -EPERM) _exit(2);
		/* reading needs no privilege */
		int f2 = nl_open(SOCK_RAW);
		_exit(read_links(f2) ? 3 : 0);
	}
	int st;
	waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) return 10 + (WIFEXITED(st) ? WEXITSTATUS(st) : 50);
	return 0;
}

static int t_unsupported(void)
{
	int fd = nl_open(SOCK_RAW);
	unsigned seq;
	if (ask(fd, RTM_GETNEIGH, NLM_F_DUMP, AF_UNSPEC, &seq)) return 1;
	if (read_ack(fd, seq, 0) != -EOPNOTSUPP) return 2;
	if (ask(fd, RTM_GETADDR, 0, AF_INET, &seq)) return 3;
	if (read_ack(fd, seq, 0) != -EOPNOTSUPP) return 4;
	return 0;
}

static int t_malformed(void)
{
	int fd = nl_open(SOCK_RAW);
	char b[64];
	struct nlmsghdr h = { .nlmsg_len = 1000, .nlmsg_type = RTM_GETLINK,
	                      .nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP };
	if (send(fd, &h, sizeof(h), 0) != (long)sizeof(h)) return 1;
	if (recv(fd, b, sizeof(b), MSG_DONTWAIT) != -1 || errno != EAGAIN) return 2;
	h.nlmsg_len = 4;                                       /* shorter than a header */
	if (send(fd, &h, sizeof(h), 0) != (long)sizeof(h)) return 3;
	if (recv(fd, b, sizeof(b), MSG_DONTWAIT) != -1 || errno != EAGAIN) return 4;
	if (send(fd, b, 3, 0) != 3) return 5;                  /* runt */
	if (recv(fd, b, sizeof(b), MSG_DONTWAIT) != -1 || errno != EAGAIN) return 6;

	static char huge[64 * 1024];
	if (send(fd, huge, sizeof(huge), 0) != -1 || errno != EMSGSIZE) return 7;

	struct sockaddr_nl to = { .nl_family = AF_NETLINK, .nl_pid = 4242 };
	h.nlmsg_len = sizeof(h);
	if (sendto(fd, &h, sizeof(h), 0, (void *)&to, sizeof(to)) != -1 || errno != ECONNREFUSED)
		return 8;
	if (connect(fd, (void *)&to, sizeof(to)) != -1 || errno != ECONNREFUSED) return 9;
	to.nl_pid = 0;
	if (connect(fd, (void *)&to, sizeof(to)) != 0) return 10;
	return 0;
}

/* requests that are never read back must hit a wall, not eat the kernel heap */
static int t_backlog_bounded(void)
{
	int fd = nl_open(SOCK_RAW);
	unsigned seq;
	int i;
	for (i = 0; i < 200000; i++)
		if (ask(fd, RTM_GETLINK, NLM_F_DUMP, AF_UNSPEC, &seq)) break;
	if (i == 200000) return 1;
	if (errno != ENOBUFS) return 2;
	char b[BUF];
	if (recv(fd, b, sizeof(b), 0) <= 0) return 3;  /* queue still drains */
	return 0;
}

static int count_links(int fd)
{
	if (read_links(fd)) return -1;
	return L.count;
}

static int t_netns(void)
{
	int before = nl_open(SOCK_RAW);
	int outer = count_links(before);
	int outer_eth = L.eth_index;
	if (outer < 1) return 1;

	if (unshare(CLONE_NEWNET) != 0) return 2;
	int inner = nl_open(SOCK_RAW);
	if (count_links(inner) != 1 || !L.lo_index || L.eth_index) return 3;   /* lo only */

	/* the old fd keeps reporting the namespace it was opened in */
	if (count_links(before) != outer) return 4;
	if (L.eth_index != outer_eth) return 5;

	/* port ids are per namespace */
	struct sockaddr_nl a = { .nl_family = AF_NETLINK, .nl_pid = 0x7777 };
	int c = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (bind(c, (void *)&a, sizeof(a)) != 0) return 6;
	if (unshare(CLONE_NEWNET) != 0) return 7;
	int d = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (bind(d, (void *)&a, sizeof(a)) != 0) return 8;
	return 0;
}

/* the socket pins its namespace: the only process in it may exit first */
static int t_netns_pinned(void)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv)) return 1;
	pid_t pid = fork();
	if (pid == 0) {
		if (unshare(CLONE_NEWNET) != 0) _exit(2);
		int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
		char c = 'x';
		char ctl[CMSG_SPACE(sizeof(int))];
		memset(ctl, 0, sizeof(ctl));
		struct iovec v = { &c, 1 };
		struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1,
		                    .msg_control = ctl, .msg_controllen = sizeof(ctl) };
		struct cmsghdr *h = CMSG_FIRSTHDR(&m);
		h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS;
		h->cmsg_len = CMSG_LEN(sizeof(int));
		*(int *)CMSG_DATA(h) = fd;
		_exit(sendmsg(sv[1], &m, 0) == 1 ? 0 : 3);
	}
	int st;
	waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) return 2;

	char c, ctl[CMSG_SPACE(sizeof(int))];
	struct iovec v = { &c, 1 };
	struct msghdr m = { .msg_iov = &v, .msg_iovlen = 1,
	                    .msg_control = ctl, .msg_controllen = sizeof(ctl) };
	if (recvmsg(sv[0], &m, 0) != 1) return 3;
	struct cmsghdr *h = CMSG_FIRSTHDR(&m);
	if (!h) return 4;
	int fd = *(int *)CMSG_DATA(h);
	if (count_links(fd) != 1) return 5;
	if (!L.lo_index || L.eth_index) return 6;
	close(fd);
	return 0;
}

static int t_getifaddrs(void)
{
	struct ifaddrs *list;
	if (getifaddrs(&list) != 0) return 1;
	struct ifreq q;
	memset(&q, 0, sizeof(q));
	strcpy(q.ifr_name, "lo");
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (ioctl(s, SIOCGIFTXQLEN, &q) != 0) return 6;      /* `ip` asks this for every link */
	q.ifr_flags = 0;
	if (ioctl(s, SIOCSIFFLAGS, &q) != -1 || errno != EOPNOTSUPP) return 7;
	int lo4 = 0, lo_pkt = 0;
	for (struct ifaddrs *i = list; i; i = i->ifa_next) {
		if (strcmp(i->ifa_name, "lo") != 0) continue;
		if (!(i->ifa_flags & IFF_LOOPBACK)) return 2;
		if (!i->ifa_addr) continue;
		if (i->ifa_addr->sa_family == AF_PACKET) lo_pkt = 1;
		if (i->ifa_addr->sa_family == AF_INET &&
		    ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
			lo4 = 1;
			if (!i->ifa_netmask ||
			    ((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr != htonl(0xff000000))
				return 3;
		}
	}
	freeifaddrs(list);
	if (!lo4 || !lo_pkt) return 4;
	if (if_nametoindex("lo") == 0) return 5;
	return 0;
}

static int run(const char *name, int (*fn)(void))
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) { alarm(10); _exit(fn() & 0xff); }
	int st;
	waitpid(pid, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0) { printf("ok   %s\n", name); return 0; }
	if (WIFSIGNALED(st)) printf("FAIL %-14s sig %d\n", name, WTERMSIG(st));
	else                 printf("FAIL %-14s step %d\n", name, WEXITSTATUS(st));
	return 1;
}

int main(void)
{
	static const struct { const char *n; int (*f)(void); } c[] = {
		{ "socket",       t_socket },
		{ "bind-names",   t_bind_names },
		{ "autobind",     t_autobind_on_send },
		{ "link-dump",    t_link_dump },
		{ "addr-dump",    t_addr_dump },
		{ "route-dump",   t_route_dump },
		{ "sendmsg-iov",  t_sendmsg_iovecs },
		{ "write-read",   t_write_read },
		{ "trunc-peek",   t_truncate_peek },
		{ "nonblocking",  t_nonblocking },
		{ "single-get",   t_single_get },
		{ "route-get",    t_route_get },
		{ "mutate",       t_mutate },
		{ "unsupported",  t_unsupported },
		{ "malformed",    t_malformed },
		{ "backlog",      t_backlog_bounded },
		{ "netns",        t_netns },
		{ "netns-pinned", t_netns_pinned },
		{ "getifaddrs",   t_getifaddrs },
	};
	int fails = 0;
	for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++)
		fails += run(c[i].n, c[i].f);
	if (fails) printf("netlink-fail %d\n", fails);
	else       printf("OK\n");
	return 0;
}
