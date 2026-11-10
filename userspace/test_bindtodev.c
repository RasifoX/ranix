/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* SO_BINDTODEVICE: a socket bound to a device sends through it, along that
 * device's routes, whatever the main table prefers (`ping -I wlan0`). Needs the
 * test VM's two NICs: eth0 10.0.2.15/24 (gw 10.0.2.2), eth1 10.0.3.15/24
 * (gw 10.0.3.2); eth0's default wins by metric. Where packets really leave is
 * read from the tx counters in /proc/net/dev. Last line: OK. */
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
#include <sys/ioctl.h>
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

static long t0, t1;
static void mark(void) { t0 = tx_packets("eth0"); t1 = tx_packets("eth1"); }
/* 0 = only eth0's tx grew, 1 = only eth1's, -1 otherwise */
static int grew(void)
{
	usleep(100000);
	long d0 = tx_packets("eth0") - t0, d1 = tx_packets("eth1") - t1;
	if (d0 > 0 && d1 == 0) return 0;
	if (d1 > 0 && d0 == 0) return 1;
	printf("  eth0 +%ld eth1 +%ld\n", d0, d1);
	return -1;
}

static int bind_dev(int s, const char *dev)
{
	return setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, dev, (socklen_t)strlen(dev) + 1) ? -errno : 0;
}

static struct sockaddr_in at(const char *ip, int port)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((unsigned short)port) };
	inet_pton(AF_INET, ip, &a.sin_addr);
	return a;
}

/* 3 UDP datagrams from a socket bound to dev ("" = not bound) */
static int udp_via(const char *dev, const char *ip)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (*dev && bind_dev(s, dev)) { close(s); return -2; }
	struct sockaddr_in to = at(ip, 9);
	mark();
	for (int i = 0; i < 3; i++)
		if (sendto(s, "x", 1, 0, (void *)&to, sizeof(to)) != 1) { close(s); return -3; }
	close(s);
	return grew();
}

/* ── cases ───────────────────────────────────────────────────────────────── */

static int t_set_get(void)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (bind_dev(s, "eth1")) return 1;
	char name[IF_NAMESIZE] = "";
	socklen_t l = sizeof(name);
	if (getsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, name, &l)) return 2;
	if (strcmp(name, "eth1") != 0 || l != 5) return 3;
	if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, "", 0)) return 4;   /* unbind */
	l = sizeof(name);
	if (getsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, name, &l) || l != 0) return 5;
	if (bind_dev(s, "nosuch0") != -ENODEV) return 6;
	close(s);
	return 0;
}

static int t_nonroot(void)
{
	if (setuid(1000) != 0) return 1;
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (bind_dev(s, "eth1") != -EPERM) return 2;
	return 0;
}

/* the table sends 10.0.2.2 out eth0; bound to eth1 it leaves eth1 (via eth1's
 * own default, 10.0.3.2), and the other way round for eth1's subnet */
static int t_udp(void)
{
	if (udp_via("", "10.0.2.2") != 0) return 1;
	if (udp_via("eth1", "10.0.2.2") != 1) return 2;
	if (udp_via("", "10.0.3.2") != 1) return 3;
	if (udp_via("eth0", "10.0.3.2") != 0) return 4;
	return 0;
}

static unsigned short cksum(const void *p, int n)
{
	const unsigned short *w = p;
	unsigned long sum = 0;
	for (; n > 1; n -= 2) sum += *w++;
	if (n) sum += *(const unsigned char *)w;
	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
	return (unsigned short)~sum;
}

/* raw ICMP echo, as busybox ping does it: 10.0.3.2 answers through eth1 */
static int t_icmp(void)
{
	int s = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
	if (s < 0) return 1;
	if (bind_dev(s, "eth1")) return 2;
	struct icmphdr h = { .type = ICMP_ECHO };
	h.un.echo.id = htons(0x4e54);
	h.un.echo.sequence = htons(1);
	h.checksum = cksum(&h, sizeof(h));
	struct sockaddr_in to = at("10.0.2.2", 0);
	mark();
	if (sendto(s, &h, sizeof(h), 0, (void *)&to, sizeof(to)) != (long)sizeof(h)) return 3;
	if (grew() != 1) return 4;
	to = at("10.0.3.2", 0);
	for (int tries = 0; tries < 3; tries++) {
		if (sendto(s, &h, sizeof(h), 0, (void *)&to, sizeof(to)) != (long)sizeof(h)) return 5;
		struct pollfd p = { s, POLLIN, 0 };
		while (poll(&p, 1, 1000) == 1) {
			char b[256];
			long n = recv(s, b, sizeof(b), 0);
			if (n >= 28 && b[20] == ICMP_ECHOREPLY) return 0;
		}
	}
	return 6;
}

/* a TCP SYN from a bound socket leaves the bound device */
static int t_tcp(void)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (bind_dev(s, "eth1")) return 1;
	fcntl(s, F_SETFL, O_NONBLOCK);
	struct sockaddr_in to = at("10.0.2.2", 7);
	mark();
	if (connect(s, (void *)&to, sizeof(to)) == 0 || errno != EINPROGRESS) return 2;
	int r = grew();
	close(s);
	return r == 1 ? 0 : 3;
}

/* bound to a device that is down: no way out, not a silent detour */
static int t_down(void)
{
	struct ifreq q;
	memset(&q, 0, sizeof(q));
	strcpy(q.ifr_name, "eth1");
	int c = socket(AF_INET, SOCK_DGRAM, 0);
	ioctl(c, SIOCGIFFLAGS, &q);
	q.ifr_flags &= ~IFF_UP;
	if (ioctl(c, SIOCSIFFLAGS, &q)) return 1;
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	int r = 0;
	if (bind_dev(s, "eth1")) r = 2;
	struct sockaddr_in to = at("10.0.2.2", 9);
	mark();
	if (!r && (sendto(s, "x", 1, 0, (void *)&to, sizeof(to)) != -1 || errno != ENETUNREACH)) r = 3;
	if (!r && tx_packets("eth0") != t0) r = 4;
	q.ifr_flags |= IFF_UP;
	if (ioctl(c, SIOCSIFFLAGS, &q)) return 5;
	close(s);
	close(c);
	return r;
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
		{ "set-get",  t_set_get },
		{ "nonroot",  t_nonroot },
		{ "udp",      t_udp },
		{ "icmp",     t_icmp },
		{ "tcp",      t_tcp },
		{ "down",     t_down },
	};
	if (!if_nametoindex("eth1")) { printf("no eth1: the test VM needs two NICs\n"); return 0; }
	int fails = 0;
	for (unsigned i = 0; i < sizeof(c) / sizeof(c[0]); i++)
		fails += run(c[i].n, c[i].f);
	if (fails) printf("bindtodev-fail %d\n", fails);
	else       printf("OK\n");
	return 0;
}
