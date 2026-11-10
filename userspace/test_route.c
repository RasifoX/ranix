/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The IPv4 routing table, read side. Needs the test VM's two NICs:
 * eth0 10.0.2.15/24 (gw 10.0.2.2) and eth1 10.0.3.15/24 (gw 10.0.3.2).
 * Which device a datagram really leaves through is read from the tx counters
 * in /proc/net/dev. Prints one line per case; the last line is OK. */
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
#include <sys/socket.h>
#include <sys/wait.h>

static unsigned seqno = 1;

/* tx packets of one device, from /proc/net/dev; -1 if absent */
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

/* send n UDP datagrams to ip:9 and report each device's tx growth */
static int udp_burst(const char *ip, int n, long *d_eth0, long *d_eth1)
{
	long a0 = tx_packets("eth0"), a1 = tx_packets("eth1");
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(9) };
	inet_pton(AF_INET, ip, &to.sin_addr);
	for (int i = 0; i < n; i++) sendto(s, "x", 1, 0, (void *)&to, sizeof(to));
	close(s);
	usleep(100000);
	*d_eth0 = tx_packets("eth0") - a0;
	*d_eth1 = tx_packets("eth1") - a1;
	return 0;
}

/* ── netlink ─────────────────────────────────────────────────────────────── */

typedef struct { int oif; unsigned dst, gw, prio, have_prio; int dst_len, table, proto; } rt_t;

static void parse_rt(struct nlmsghdr *h, rt_t *o)
{
	struct rtmsg *r = NLMSG_DATA(h);
	memset(o, 0, sizeof(*o));
	o->dst_len = r->rtm_dst_len; o->table = r->rtm_table; o->proto = r->rtm_protocol;
	long len = (long)h->nlmsg_len - NLMSG_LENGTH(sizeof(*r));
	for (struct rtattr *a = RTM_RTA(r); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
		unsigned v = *(unsigned *)RTA_DATA(a);
		switch (a->rta_type) {
		case RTA_OIF:      o->oif = (int)v; break;
		case RTA_DST:      o->dst = ntohl(v); break;
		case RTA_GATEWAY:  o->gw = ntohl(v); break;
		case RTA_PRIORITY: o->prio = v; o->have_prio = 1; break;
		}
	}
}

static int dump_routes(rt_t *out, int max)
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	struct { struct nlmsghdr h; struct rtmsg r; } q;
	memset(&q, 0, sizeof(q));
	q.h.nlmsg_len = sizeof(q);
	q.h.nlmsg_type = RTM_GETROUTE;
	q.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	q.h.nlmsg_seq = ++seqno;
	q.r.rtm_family = AF_INET;
	if (send(fd, &q, sizeof(q), 0) != (long)sizeof(q)) { close(fd); return -1; }
	int n = 0;
	static char b[16384];
	for (;;) {
		long len = recv(fd, b, sizeof(b), 0);
		if (len <= 0) break;
		for (struct nlmsghdr *h = (void *)b; NLMSG_OK(h, (unsigned long)len); h = NLMSG_NEXT(h, len)) {
			if (h->nlmsg_type == NLMSG_DONE) { close(fd); return n; }
			if (h->nlmsg_type == NLMSG_ERROR) { close(fd); return -2; }
			if (h->nlmsg_type != RTM_NEWROUTE || n >= max) continue;
			parse_rt(h, &out[n]);
			if (out[n].table == RT_TABLE_MAIN) n++;
		}
	}
	close(fd);
	return -3;
}

/* `ip route get dst [oif]`: 0 and *o filled, or -errno */
static int route_get(const char *dst, int oif, rt_t *o)
{
	int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	struct { struct nlmsghdr h; struct rtmsg r; struct rtattr a; unsigned d; struct rtattr b; int oif; } q;
	memset(&q, 0, sizeof(q));
	q.h.nlmsg_len = oif ? sizeof(q) : offsetof(__typeof__(q), b);
	q.h.nlmsg_type = RTM_GETROUTE;
	q.h.nlmsg_flags = NLM_F_REQUEST;
	q.h.nlmsg_seq = ++seqno;
	q.r.rtm_family = AF_INET;
	q.r.rtm_dst_len = 32;
	q.a.rta_type = RTA_DST; q.a.rta_len = RTA_LENGTH(4);
	inet_pton(AF_INET, dst, &q.d);
	q.b.rta_type = RTA_OIF; q.b.rta_len = RTA_LENGTH(4); q.oif = oif;
	if (send(fd, &q, q.h.nlmsg_len, 0) != (long)q.h.nlmsg_len) { close(fd); return -1000; }
	char b[1024];
	long n = recv(fd, b, sizeof(b), 0);
	close(fd);
	struct nlmsghdr *h = (void *)b;
	if (n <= 0 || !NLMSG_OK(h, (unsigned long)n)) return -1001;
	if (h->nlmsg_type == NLMSG_ERROR) return ((struct nlmsgerr *)NLMSG_DATA(h))->error;
	parse_rt(h, o);
	return 0;
}

static unsigned ip4(const char *s) { struct in_addr a; inet_pton(AF_INET, s, &a); return ntohl(a.s_addr); }

/* ── cases ───────────────────────────────────────────────────────────────── */

static int t_two_nics(void)
{
	if (!if_nametoindex("eth0")) return 1;
	if (!if_nametoindex("eth1")) return 2;
	return 0;
}

/* `ip route`: each NIC's connected route and DHCP default, eth0's default first
 * by metric */
static int t_dump(void)
{
	rt_t r[32];
	int n = dump_routes(r, 32);
	if (n < 0) return 1;
	int e0 = (int)if_nametoindex("eth0"), e1 = (int)if_nametoindex("eth1");
	int c0 = 0, c1 = 0;
	unsigned m0 = 0, m1 = 0;
	int d0 = 0, d1 = 0;
	for (int i = 0; i < n; i++) {
		if (r[i].dst_len == 24 && r[i].dst == ip4("10.0.2.0") && r[i].oif == e0 && !r[i].gw) c0 = 1;
		if (r[i].dst_len == 24 && r[i].dst == ip4("10.0.3.0") && r[i].oif == e1 && !r[i].gw) c1 = 1;
		if (r[i].dst_len == 0 && r[i].gw == ip4("10.0.2.2") && r[i].oif == e0 && r[i].have_prio) { d0 = 1; m0 = r[i].prio; }
		if (r[i].dst_len == 0 && r[i].gw == ip4("10.0.3.2") && r[i].oif == e1 && r[i].have_prio) { d1 = 1; m1 = r[i].prio; }
	}
	if (!c0) return 2;
	if (!c1) return 3;
	if (!d0) return 4;
	if (!d1) return 5;
	if (m0 >= m1) return 6;
	return 0;
}

/* a destination on eth1's subnet leaves eth1, even though eth0 has the default */
static int t_connected_wins(void)
{
	long d0, d1;
	udp_burst("10.0.3.2", 3, &d0, &d1);
	if (d1 < 1) { printf("  eth0 +%ld eth1 +%ld\n", d0, d1); return 1; }
	if (d0 != 0) { printf("  eth0 +%ld eth1 +%ld\n", d0, d1); return 2; }
	return 0;
}

/* anything else follows the lowest-metric default: eth0 */
static int t_default(void)
{
	long d0, d1;
	udp_burst("10.0.2.2", 3, &d0, &d1);
	if (d0 < 1 || d1 != 0) { printf("  eth0 +%ld eth1 +%ld\n", d0, d1); return 1; }
	return 0;
}

static int t_get(void)
{
	int e0 = (int)if_nametoindex("eth0"), e1 = (int)if_nametoindex("eth1");
	rt_t o;
	if (route_get("10.0.3.77", 0, &o)) return 1;
	if (o.oif != e1 || o.gw) return 2;
	if (route_get("192.0.2.1", 0, &o)) return 3;
	if (o.oif != e0 || o.gw != ip4("10.0.2.2")) return 4;
	if (route_get("192.0.2.1", e1, &o)) return 5;   /* `ip route get ... oif eth1` */
	if (o.oif != e1 || o.gw != ip4("10.0.3.2")) return 6;
	return 0;
}

/* /proc/net/route, as net-tools and fjord's lib/network read it */
static int t_proc(void)
{
	FILE *f = fopen("/proc/net/route", "r");
	if (!f) return 1;
	char l[256];
	int def0 = 0, net1 = 0;
	while (fgets(l, sizeof(l), f)) {
		if (strncmp(l, "eth0\t00000000\t0202000A", 22) == 0) def0 = 1;
		if (strncmp(l, "eth1\t0003000A\t00000000", 22) == 0) net1 = 1;
	}
	fclose(f);
	if (!def0) return 2;
	if (!net1) return 3;
	return 0;
}

/* a connected UDP socket's source is its route's device address */
static int t_udp_source(void)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(9) };
	inet_pton(AF_INET, "10.0.3.2", &to.sin_addr);
	if (connect(s, (void *)&to, sizeof(to))) return 1;
	struct sockaddr_in me;
	socklen_t l = sizeof(me);
	if (getsockname(s, (void *)&me, &l)) return 2;
	close(s);
	if (ntohl(me.sin_addr.s_addr) != ip4("10.0.3.15")) return 3;
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
	if (WIFSIGNALED(st)) printf("FAIL %-16s sig %d\n", name, WTERMSIG(st));
	else                 printf("FAIL %-16s step %d\n", name, WEXITSTATUS(st));
	return 1;
}

int main(void)
{
	static const struct { const char *n; int (*f)(void); } c[] = {
		{ "two-nics",       t_two_nics },
		{ "dump",           t_dump },
		{ "connected-wins", t_connected_wins },
		{ "default",        t_default },
		{ "get",            t_get },
		{ "proc-route",     t_proc },
		{ "udp-source",     t_udp_source },
	};
	int fails = 0;
	for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++)
		fails += run(c[i].n, c[i].f);
	if (fails) printf("route-fail %d\n", fails);
	else       printf("OK\n");
	return 0;
}
