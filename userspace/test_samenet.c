/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Two devices on one subnet, as cable and Wi-Fi at home: eth0 10.0.2.15 and
 * eth2 10.0.2.16, each behind its own slirp with the same router 10.0.2.2.
 * Only eth0's slirp echoes TCP at 10.0.2.100:7, so a TCP echo there proves the
 * connection used eth0 both ways. Where packets leave is read from the tx
 * counters in /proc/net/dev. The last line is OK. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <sys/wait.h>

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

static long t0, t2;
static void mark(void) { t0 = tx_packets("eth0"); t2 = tx_packets("eth2"); }
/* 0 = only eth0's tx grew, 2 = only eth2's, -1 otherwise */
static int grew(void)
{
	usleep(100000);
	long d0 = tx_packets("eth0") - t0, d2 = tx_packets("eth2") - t2;
	if (d0 > 0 && d2 == 0) return 0;
	if (d2 > 0 && d0 == 0) return 2;
	printf("  eth0 +%ld eth2 +%ld\n", d0, d2);
	return -1;
}

static struct sockaddr_in at(const char *ip, int port)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((unsigned short)port) };
	inet_pton(AF_INET, ip, &a.sin_addr);
	return a;
}

/* TCP echo through 10.0.2.100:7 (eth0's slirp only), from src ("" = any) */
static int tcp_echo(const char *src)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (*src) {
		struct sockaddr_in me = at(src, 0);
		if (bind(s, (void *)&me, sizeof(me))) { close(s); return 1; }
	}
	fcntl(s, F_SETFL, O_NONBLOCK);
	struct sockaddr_in to = at("10.0.2.100", 7);
	if (connect(s, (void *)&to, sizeof(to)) && errno != EINPROGRESS) { close(s); return 2; }
	struct pollfd p = { s, POLLOUT, 0 };
	if (poll(&p, 1, 3000) != 1) { close(s); return 3; }
	int err = 0;
	socklen_t l = sizeof(err);
	getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &l);
	if (err) { close(s); return 4; }
	if (send(s, "ranix", 5, 0) != 5) { close(s); return 5; }
	char b[16];
	long got = 0;
	p.events = POLLIN;
	while (got < 5 && poll(&p, 1, 3000) == 1) {
		long n = recv(s, b + got, sizeof(b) - (size_t)got, 0);
		if (n <= 0) break;
		got += n;
	}
	close(s);
	return got == 5 && memcmp(b, "ranix", 5) == 0 ? 0 : 6;
}

/* `ip route add|del 10.0.2.100/32 dev eth2` */
static int host_route(int type)
{
	struct { struct nlmsghdr h; struct rtmsg r; struct rtattr a; unsigned d; struct rtattr b; int oif; } q;
	memset(&q, 0, sizeof(q));
	q.h.nlmsg_len = sizeof(q);
	q.h.nlmsg_type = (unsigned short)type;
	q.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK | (type == RTM_NEWROUTE ? NLM_F_CREATE | NLM_F_EXCL : 0);
	q.h.nlmsg_seq = 7;
	q.r.rtm_family = AF_INET;
	q.r.rtm_dst_len = 32;
	q.r.rtm_table = RT_TABLE_MAIN;
	if (type == RTM_NEWROUTE) { q.r.rtm_protocol = RTPROT_BOOT; q.r.rtm_scope = RT_SCOPE_LINK; q.r.rtm_type = RTN_UNICAST; }
	q.a.rta_type = RTA_DST; q.a.rta_len = RTA_LENGTH(4); inet_pton(AF_INET, "10.0.2.100", &q.d);
	q.b.rta_type = RTA_OIF; q.b.rta_len = RTA_LENGTH(4); q.oif = (int)if_nametoindex("eth2");
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	send(fd, &q, sizeof(q), 0);
	char b[512];
	long n = recv(fd, b, sizeof(b), 0);
	close(fd);
	struct nlmsghdr *h = (void *)b;
	if (n <= 0 || h->nlmsg_type != NLMSG_ERROR) return -1000;
	return ((struct nlmsgerr *)NLMSG_DATA(h))->error;
}

/* ── cases ───────────────────────────────────────────────────────────────── */

/* each device keeps its connected route; eth0's wins by metric */
static int t_both_connected(void)
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	struct { struct nlmsghdr h; struct rtmsg r; } q;
	memset(&q, 0, sizeof(q));
	q.h.nlmsg_len = sizeof(q);
	q.h.nlmsg_type = RTM_GETROUTE;
	q.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	q.r.rtm_family = AF_INET;
	send(fd, &q, sizeof(q), 0);
	int e0 = (int)if_nametoindex("eth0"), e2 = (int)if_nametoindex("eth2");
	long m0 = -1, m2 = -1;
	static char b[16384];
	for (int done = 0; !done;) {
		long len = recv(fd, b, sizeof(b), 0);
		if (len <= 0) break;
		for (struct nlmsghdr *h = (void *)b; NLMSG_OK(h, (unsigned long)len); h = NLMSG_NEXT(h, len)) {
			if (h->nlmsg_type != RTM_NEWROUTE) { done = 1; break; }
			struct rtmsg *r = NLMSG_DATA(h);
			long al = (long)h->nlmsg_len - NLMSG_LENGTH(sizeof(*r));
			unsigned dst = 0, prio = 0; int oif = 0, gw = 0;
			for (struct rtattr *a = RTM_RTA(r); RTA_OK(a, al); a = RTA_NEXT(a, al)) {
				if (a->rta_type == RTA_DST) dst = ntohl(*(unsigned *)RTA_DATA(a));
				if (a->rta_type == RTA_OIF) oif = *(int *)RTA_DATA(a);
				if (a->rta_type == RTA_PRIORITY) prio = *(unsigned *)RTA_DATA(a);
				if (a->rta_type == RTA_GATEWAY) gw = 1;
			}
			if (r->rtm_table != RT_TABLE_MAIN || r->rtm_dst_len != 24 || gw || dst != 0x0a000200) continue;
			if (oif == e0) m0 = prio;
			if (oif == e2) m2 = prio;
		}
	}
	close(fd);
	if (m0 < 0) return 1;
	if (m2 < 0) return 2;
	if (m0 >= m2) return 3;
	return 0;
}

/* TCP to the subnet, with eth2 up on it too, still goes (and comes back) on eth0 */
static int t_tcp_eth0(void) { return tcp_echo(""); }

/* a socket bound to eth0's address leaves eth0 even when the table prefers eth2
 * for that host (a reply's source names its device) */
static int t_tcp_by_source(void)
{
	if (host_route(RTM_NEWROUTE)) return 1;
	int r = tcp_echo("10.0.2.15") ? 2 : 0;
	if (host_route(RTM_DELROUTE)) return 3;
	return r;
}

static int t_udp(void)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in to = at("10.0.2.2", 9);
	mark();
	for (int i = 0; i < 3; i++) sendto(s, "x", 1, 0, (void *)&to, sizeof(to));
	if (grew() != 0) return 1;                       /* unbound: eth0 by metric */
	close(s);
	s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in me = at("10.0.2.16", 0);
	if (bind(s, (void *)&me, sizeof(me))) return 2;
	mark();
	for (int i = 0; i < 3; i++) sendto(s, "x", 1, 0, (void *)&to, sizeof(to));
	if (grew() != 2) return 3;                       /* eth2's address: eth2 */
	close(s);
	return 0;
}

static unsigned short cksum(const void *p, int n)
{
	const unsigned short *w = p;
	unsigned long sum = 0;
	for (; n > 1; n -= 2) sum += *w++;
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return (unsigned short)~sum;
}

/* `ping -I eth2 10.0.2.2`: only eth2 sends, and the answer comes */
static int t_ping_eth2(void)
{
	int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, "eth2", 5)) return 1;
	struct icmphdr h = { .type = ICMP_ECHO };
	h.un.echo.id = htons(0x5341);
	struct sockaddr_in to = at("10.0.2.2", 0);
	int replies = 0;
	mark();
	for (int i = 0; i < 3; i++) {
		h.un.echo.sequence = htons((unsigned short)i);
		h.checksum = 0;
		h.checksum = cksum(&h, sizeof(h));
		if (sendto(s, &h, sizeof(h), 0, (void *)&to, sizeof(to)) != (long)sizeof(h)) return 2;
		struct pollfd p = { s, POLLIN, 0 };
		while (poll(&p, 1, 1000) == 1) {
			char b[256];
			long n = recv(s, b, sizeof(b), 0);
			if (n >= 28 && b[20] == ICMP_ECHOREPLY) { replies++; break; }
		}
	}
	if (grew() != 2) return 3;
	if (replies < 2) return 4;
	return 0;
}

static int run(const char *name, int (*fn)(void))
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) { alarm(20); _exit(fn() & 0xff); }
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
		{ "both-connected", t_both_connected },
		{ "tcp-eth0",       t_tcp_eth0 },
		{ "tcp-by-source",  t_tcp_by_source },
		{ "udp",            t_udp },
		{ "ping-I-eth2",    t_ping_eth2 },
	};
	if (!if_nametoindex("eth2")) { printf("no eth2: the test VM needs three NICs\n"); return 0; }
	int fails = 0;
	for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++)
		fails += run(c[i].n, c[i].f);
	if (fails) printf("samenet-fail %d\n", fails);
	else       printf("OK\n");
	return 0;
}
