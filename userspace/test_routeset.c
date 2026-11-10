/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The IPv4 routing table, write side: rtnetlink RTM_NEWROUTE/RTM_DELROUTE,
 * link up/down by SIOCSIFFLAGS and RTM_NEWLINK, root only. Needs the test VM's
 * two NICs: eth0 10.0.2.15/24 (gw 10.0.2.2), eth1 10.0.3.15/24 (gw 10.0.3.2).
 * Every case puts eth1 and the table back as it found them. Where a datagram
 * really leaves is read from the tx counters in /proc/net/dev. Last line: OK. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>

static unsigned seqno = 1;

static long tx_packets(const char *dev)
{
	FILE *f = fopen("/proc/net/dev", "r");
	if (!f) return -1;
	char line[512];
	long tx = -1;
	while (fgets(line, sizeof(line), f)) {
		char *c = strchr(line, ':');
		if (!c) continue;
		*c = 0;
		char *name = line;
		while (*name == ' ') name++;
		if (strcmp(name, dev) != 0) continue;
		unsigned long v[10];
		if (sscanf(c + 1, "%lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
		           &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]) == 10)
			tx = (long)v[9];
	}
	fclose(f);
	return tx;
}

/* 3 UDP datagrams to ip:9: which device's tx grew? 0 eth0, 1 eth1, -1 neither
 * or both, -2 the send failed (errno kept) */
static int leaves_via(const char *ip)
{
	long a0 = tx_packets("eth0"), a1 = tx_packets("eth1");
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(9) };
	inet_pton(AF_INET, ip, &to.sin_addr);
	int bad = 0;
	for (int i = 0; i < 3; i++)
		if (sendto(s, "x", 1, 0, (void *)&to, sizeof(to)) != 1) bad = 1;
	int e = errno;
	close(s);
	if (bad) { errno = e; return -2; }
	usleep(100000);
	long d0 = tx_packets("eth0") - a0, d1 = tx_packets("eth1") - a1;
	if (d0 > 0 && d1 == 0) return 0;
	if (d1 > 0 && d0 == 0) return 1;
	printf("  %s: eth0 +%ld eth1 +%ld\n", ip, d0, d1);
	return -1;
}

static unsigned ip4(const char *s) { struct in_addr a; inet_pton(AF_INET, s, &a); return a.s_addr; }

/* ── netlink requests ────────────────────────────────────────────────────── */

typedef struct { struct nlmsghdr h; char body[256]; } req_t;

static void add_attr(req_t *q, int type, const void *data, int len)
{
	struct rtattr *a = (void *)((char *)&q->h + NLMSG_ALIGN(q->h.nlmsg_len));
	a->rta_type = (unsigned short)type;
	a->rta_len = (unsigned short)RTA_LENGTH(len);
	memcpy(RTA_DATA(a), data, (size_t)len);
	q->h.nlmsg_len = NLMSG_ALIGN(q->h.nlmsg_len) + RTA_ALIGN(a->rta_len);
}

/* send one request, return the ack's error (0 = done) */
static int talk(req_t *q)
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	q->h.nlmsg_flags |= NLM_F_REQUEST | NLM_F_ACK;
	q->h.nlmsg_seq = ++seqno;
	if (send(fd, q, q->h.nlmsg_len, 0) != (long)q->h.nlmsg_len) { close(fd); return -1000; }
	char b[1024];
	long n = recv(fd, b, sizeof(b), 0);
	close(fd);
	struct nlmsghdr *h = (void *)b;
	if (n <= 0 || !NLMSG_OK(h, (unsigned long)n) || h->nlmsg_type != NLMSG_ERROR) return -1001;
	return ((struct nlmsgerr *)NLMSG_DATA(h))->error;
}

/* `ip route add|del DST/LEN [via GW] [dev DEV] [metric M]`; NULL/0 = left out */
static int route(int type, int flags, const char *dst, int len, const char *gw,
                 const char *dev, unsigned metric)
{
	req_t q;
	memset(&q, 0, sizeof(q));
	q.h.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	q.h.nlmsg_type = (unsigned short)type;
	q.h.nlmsg_flags = (unsigned short)flags;
	struct rtmsg *r = NLMSG_DATA(&q.h);
	r->rtm_family = AF_INET;
	r->rtm_dst_len = (unsigned char)len;
	r->rtm_table = RT_TABLE_MAIN;
	if (type == RTM_NEWROUTE) {
		r->rtm_protocol = RTPROT_BOOT;
		r->rtm_scope = gw ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
		r->rtm_type = RTN_UNICAST;
	}
	unsigned a;
	if (len) { a = ip4(dst); add_attr(&q, RTA_DST, &a, 4); }
	if (gw)  { a = ip4(gw);  add_attr(&q, RTA_GATEWAY, &a, 4); }
	if (dev) { int i = (int)if_nametoindex(dev); add_attr(&q, RTA_OIF, &i, 4); }
	if (metric) add_attr(&q, RTA_PRIORITY, &metric, 4);
	return talk(&q);
}

#define ADD(...) route(RTM_NEWROUTE, NLM_F_CREATE | NLM_F_EXCL, __VA_ARGS__)
#define DEL(...) route(RTM_DELROUTE, 0, __VA_ARGS__)

/* `ip route get DST`: its device's index in *out, or 0 */
static int route_dev(const char *dst, int *out)
{
	req_t q;
	memset(&q, 0, sizeof(q));
	q.h.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	q.h.nlmsg_type = RTM_GETROUTE;
	q.h.nlmsg_flags = NLM_F_REQUEST;
	q.h.nlmsg_seq = ++seqno;
	struct rtmsg *r = NLMSG_DATA(&q.h);
	r->rtm_family = AF_INET;
	r->rtm_dst_len = 32;
	unsigned a = ip4(dst);
	add_attr(&q, RTA_DST, &a, 4);
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	send(fd, &q, q.h.nlmsg_len, 0);
	char b[1024];
	long n = recv(fd, b, sizeof(b), 0);
	close(fd);
	*out = 0;
	struct nlmsghdr *h = (void *)b;
	if (n <= 0 || !NLMSG_OK(h, (unsigned long)n)) return -1;
	if (h->nlmsg_type == NLMSG_ERROR) return ((struct nlmsgerr *)NLMSG_DATA(h))->error;
	struct rtmsg *m = NLMSG_DATA(h);
	long len = (long)h->nlmsg_len - NLMSG_LENGTH(sizeof(*m));
	for (struct rtattr *at = RTM_RTA(m); RTA_OK(at, len); at = RTA_NEXT(at, len))
		if (at->rta_type == RTA_OIF) *out = *(int *)RTA_DATA(at);
	return 0;
}

static int routed_to(const char *dst, const char *dev)
{
	int d;
	return route_dev(dst, &d) == 0 && d == (int)if_nametoindex(dev);
}

/* how many main-table IPv4 routes go through dev */
static int routes_on(const char *dev)
{
	int idx = (int)if_nametoindex(dev), cnt = 0;
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	req_t q;
	memset(&q, 0, sizeof(q));
	q.h.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	q.h.nlmsg_type = RTM_GETROUTE;
	q.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	q.h.nlmsg_seq = ++seqno;
	((struct rtmsg *)NLMSG_DATA(&q.h))->rtm_family = AF_INET;
	send(fd, &q, q.h.nlmsg_len, 0);
	static char b[16384];
	for (;;) {
		long len = recv(fd, b, sizeof(b), 0);
		if (len <= 0) break;
		for (struct nlmsghdr *h = (void *)b; NLMSG_OK(h, (unsigned long)len); h = NLMSG_NEXT(h, len)) {
			if (h->nlmsg_type != RTM_NEWROUTE) { close(fd); return cnt; }
			struct rtmsg *m = NLMSG_DATA(h);
			long al = (long)h->nlmsg_len - NLMSG_LENGTH(sizeof(*m));
			for (struct rtattr *at = RTM_RTA(m); RTA_OK(at, al); at = RTA_NEXT(at, al))
				if (at->rta_type == RTA_OIF && *(int *)RTA_DATA(at) == idx &&
				    m->rtm_table == RT_TABLE_MAIN) cnt++;
		}
	}
	close(fd);
	return cnt;
}

/* SIOCGIFFLAGS / SIOCSIFFLAGS IFF_UP, as busybox `ip link set` does it */
static int if_flags(const char *dev)
{
	struct ifreq r;
	memset(&r, 0, sizeof(r));
	strcpy(r.ifr_name, dev);
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	int rc = ioctl(s, SIOCGIFFLAGS, &r);
	close(s);
	return rc ? -1 : r.ifr_flags;
}

static int set_up(const char *dev, int up)
{
	struct ifreq r;
	memset(&r, 0, sizeof(r));
	strcpy(r.ifr_name, dev);
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	ioctl(s, SIOCGIFFLAGS, &r);
	if (up) r.ifr_flags |= IFF_UP;
	else    r.ifr_flags &= ~IFF_UP;
	int rc = ioctl(s, SIOCSIFFLAGS, &r) ? -errno : 0;
	close(s);
	return rc;
}

/* iproute2's way: RTM_NEWLINK, ifi_change = IFF_UP */
static int nl_set_up(const char *dev, int up)
{
	req_t q;
	memset(&q, 0, sizeof(q));
	q.h.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
	q.h.nlmsg_type = RTM_NEWLINK;
	struct ifinfomsg *i = NLMSG_DATA(&q.h);
	i->ifi_index = (int)if_nametoindex(dev);
	i->ifi_change = IFF_UP;
	i->ifi_flags = up ? IFF_UP : 0;
	return talk(&q);
}

/* ── cases ───────────────────────────────────────────────────────────────── */

static int t_nonroot(void)
{
	if (setuid(1000) != 0) return 1;
	if (ADD("192.0.2.0", 24, "10.0.3.2", "eth1", 0) != -EPERM) return 2;
	if (DEL("10.0.3.0", 24, NULL, "eth1", 0) != -EPERM) return 3;
	if (nl_set_up("eth1", 0) != -EPERM) return 4;
	if (set_up("eth1", 0) != -EPERM) return 5;
	if (!(if_flags("eth1") & IFF_UP)) return 6;
	return 0;
}

static int t_add_del(void)
{
	if (ADD("192.0.2.0", 24, "10.0.3.2", "eth1", 0)) return 1;
	int r = 0;
	if (ADD("192.0.2.0", 24, "10.0.3.2", "eth1", 0) != -EEXIST) r = 2;
	else if (!routed_to("192.0.2.9", "eth1")) r = 3;
	else if (leaves_via("192.0.2.9") != 1) r = 4;
	if (DEL("192.0.2.0", 24, NULL, NULL, 0) && !r) r = 5;
	if (r) return r;
	if (DEL("192.0.2.0", 24, NULL, NULL, 0) != -ESRCH) return 6;
	if (!routed_to("192.0.2.9", "eth0")) return 7;
	if (leaves_via("192.0.2.9") != 0) return 8;
	return 0;
}

/* the longer prefix wins whichever was added first */
static int t_longest_prefix(void)
{
	if (ADD("198.51.100.0", 24, "10.0.3.2", "eth1", 0)) return 1;
	if (ADD("198.51.100.128", 25, "10.0.2.2", "eth0", 0)) { DEL("198.51.100.0", 24, NULL, NULL, 0); return 2; }
	int r = 0;
	if (!routed_to("198.51.100.7", "eth1")) r = 3;
	else if (!routed_to("198.51.100.200", "eth0")) r = 4;
	else if (leaves_via("198.51.100.7") != 1) r = 5;
	else if (leaves_via("198.51.100.200") != 0) r = 6;
	DEL("198.51.100.0", 24, NULL, NULL, 0);
	DEL("198.51.100.128", 25, NULL, NULL, 0);
	return r;
}

/* a default with a lower metric takes over; deleting it gives eth0 back */
static int t_metric(void)
{
	if (ADD("0.0.0.0", 0, "10.0.3.2", "eth1", 50)) return 1;
	int r = 0;
	if (!routed_to("203.0.113.5", "eth1")) r = 2;
	else if (leaves_via("203.0.113.5") != 1) r = 3;
	if (DEL("0.0.0.0", 0, "10.0.3.2", "eth1", 50) && !r) r = 4;
	if (r) return r;
	if (!routed_to("203.0.113.5", "eth0")) return 5;
	if (leaves_via("203.0.113.5") != 0) return 6;
	return 0;
}

static int t_refused(void)
{
	if (ADD("203.0.113.0", 24, "10.9.9.9", NULL, 0) != -ENETUNREACH) return 1;  /* gw on no link */
	if (ADD("203.0.113.0", 24, "10.0.3.2", "eth0", 0) != -ENETUNREACH) return 2; /* not on eth0 */
	if (ADD("203.0.113.0", 24, NULL, NULL, 0) != -EINVAL) return 3;            /* no way at all */
	if (ADD("203.0.113.0", 24, "10.0.2.2", "lo", 0) != -ENETUNREACH) return 4;
	/* `ip route change` of a route that is not there */
	if (route(RTM_NEWROUTE, NLM_F_REPLACE, "203.0.113.0", 24, "10.0.2.2", "eth0", 0) != -ENOENT) return 5;
	return 0;
}

/* down: eth1's routes go, its traffic stops, a route via it is refused;
 * up: its connected route and DHCP default are back */
static int t_link_down_up(int use_netlink)
{
	int rc = use_netlink ? nl_set_up("eth1", 0) : set_up("eth1", 0);
	if (rc) return 1;
	int r = 0;
	if (if_flags("eth1") & IFF_UP) r = 2;
	else if (routes_on("eth1") != 0) r = 3;
	else if (!routed_to("10.0.3.2", "eth0")) r = 4;     /* the default carries it now */
	else if (leaves_via("10.0.3.2") != 0) r = 5;
	else if (ADD("192.0.2.0", 24, NULL, "eth1", 0) != -ENETDOWN) r = 6;
	rc = use_netlink ? nl_set_up("eth1", 1) : set_up("eth1", 1);
	if (rc) return 7;
	if (r) return r;
	if (!(if_flags("eth1") & IFF_UP)) return 8;
	if (routes_on("eth1") != 2) { printf("  eth1 routes %d\n", routes_on("eth1")); return 9; }
	if (!routed_to("10.0.3.2", "eth1")) return 10;
	if (leaves_via("10.0.3.2") != 1) return 11;
	return 0;
}
static int t_link_ioctl(void)   { return t_link_down_up(0); }
static int t_link_netlink(void) { return t_link_down_up(1); }

/* a static route does not survive its device going down (Linux does the same) */
static int t_static_flushed(void)
{
	if (ADD("192.0.2.0", 24, "10.0.3.2", "eth1", 0)) return 1;
	if (set_up("eth1", 0) || set_up("eth1", 1)) return 2;
	if (DEL("192.0.2.0", 24, NULL, NULL, 0) != -ESRCH) return 3;
	return 0;
}

static int t_lo_stays(void)
{
	if (set_up("lo", 0) != -EOPNOTSUPP) return 1;
	if (set_up("lo", 1) != 0) return 2;   /* `ip link set lo up` is harmless */
	return 0;
}

static int run(const char *name, int (*fn)(void))
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) { alarm(15); _exit(fn() & 0xff); }
	int st;
	waitpid(pid, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0) { printf("ok   %s\n", name); return 0; }
	if (WIFSIGNALED(st)) printf("FAIL %-16s sig %d\n", name, WTERMSIG(st));
	else                 printf("FAIL %-16s step %d\n", name, WEXITSTATUS(st));
	return 1;
}

int main(void)
{
	static const struct { const char *n; int (*f)(void); } c[] = {
		{ "nonroot-eperm",  t_nonroot },
		{ "add-del",        t_add_del },
		{ "longest-prefix", t_longest_prefix },
		{ "metric",         t_metric },
		{ "refused",        t_refused },
		{ "link-ioctl",     t_link_ioctl },
		{ "link-netlink",   t_link_netlink },
		{ "static-flushed", t_static_flushed },
		{ "lo-stays",       t_lo_stays },
	};
	if (!if_nametoindex("eth1")) { printf("no eth1: the test VM needs two NICs\n"); return 0; }
	int fails = 0;
	for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++)
		fails += run(c[i].n, c[i].f);
	set_up("eth1", 1);   /* whatever happened, leave eth1 up for what comes after */
	if (fails) printf("routeset-fail %d\n", fails);
	else       printf("OK\n");
	return 0;
}
