/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Forged TCP that must change nothing (a review finding). Packets are written
 * by hand through AF_PACKET on lo, so they reach ip_rx like any other:
 *   - a RST from 0.0.0.0:0 to a listening port: the listener stays (it was
 *     found as a "connection" with remote 0:0 and closed);
 *   - a SYN from 0.0.0.0:0 to it: no half-open connection to 0.0.0.0;
 *   - a RST with a guessed sequence number to a live connection: it lives on
 *     (RFC 5961: only a RST at the next expected byte resets);
 *   - a real reset (a closed port answering a SYN) still refuses.
 * Root. First failure or OK. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_packet.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 39201

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s errno=%d\n", __LINE__, #c, errno); exit(1); } } while (0)

static uint16_t csum(const void *d, size_t n, uint32_t acc)
{
	const uint8_t *b = d;
	for (size_t i = 0; i + 1 < n; i += 2) acc += (uint32_t)(b[i] << 8 | b[i + 1]);
	if (n & 1) acc += (uint32_t)b[n - 1] << 8;
	while (acc >> 16) acc = (acc & 0xffff) + (acc >> 16);
	return (uint16_t)~acc;
}

/* one IPv4 TCP segment, no options, through AF_PACKET on lo */
static void forge(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport,
                  uint32_t seq, uint32_t ack, uint8_t flags)
{
	uint8_t p[40];
	memset(p, 0, sizeof(p));
	p[0] = 0x45; p[3] = 40; p[8] = 64; p[9] = 6;
	memcpy(p + 12, &src, 4); memcpy(p + 16, &dst, 4);        /* network order already */
	uint16_t c = csum(p, 20, 0); p[10] = c >> 8; p[11] = c & 0xff;
	uint8_t *t = p + 20;
	t[0] = sport >> 8; t[1] = sport & 0xff; t[2] = dport >> 8; t[3] = dport & 0xff;
	t[4] = seq >> 24; t[5] = seq >> 16; t[6] = seq >> 8; t[7] = seq;
	t[8] = ack >> 24; t[9] = ack >> 16; t[10] = ack >> 8; t[11] = ack;
	t[12] = 5 << 4; t[13] = flags; t[14] = 0x20;              /* window 8192 */
	uint8_t ph[12 + 20];
	memcpy(ph, p + 12, 8); ph[8] = 0; ph[9] = 6; ph[10] = 0; ph[11] = 20;
	memcpy(ph + 12, t, 20);
	c = csum(ph, sizeof(ph), 0); t[16] = c >> 8; t[17] = c & 0xff;
	int s = socket(AF_PACKET, SOCK_DGRAM, htons(0x0800));
	CHECK(s >= 0);
	struct sockaddr_ll a = { .sll_family = AF_PACKET, .sll_protocol = htons(0x0800),
	                         .sll_ifindex = (int)if_nametoindex("lo"), .sll_halen = 6 };
	CHECK(a.sll_ifindex > 0);
	CHECK(sendto(s, p, sizeof(p), 0, (struct sockaddr *)&a, sizeof(a)) == (ssize_t)sizeof(p));
	close(s);
	usleep(200000);
}

static struct sockaddr_in lo_addr(int port)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	return a;
}

static int readable(int s, int ms)
{
	struct pollfd p = { .fd = s, .events = POLLIN };
	return poll(&p, 1, ms) == 1;
}

/* half-open or other connections in /proc/net/tcp whose peer is 0.0.0.0:0 */
static int zero_peers(void)
{
	FILE *f = fopen("/proc/net/tcp", "r");
	char l[256];
	int n = 0;
	if (!f) return -1;
	while (fgets(l, sizeof(l), f)) {
		char loc[64], rem[64], st[8];
		if (sscanf(l, "%*s %63s %63s %7s", loc, rem, st) == 3 && !strcmp(rem, "00000000:0000") &&
		    strcmp(st, "0A") && strcmp(st, "07"))
			n++;
	}
	fclose(f);
	return n;
}

int main(void)
{
	int one = 1;
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in la = lo_addr(PORT);
	CHECK(ls >= 0);
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	CHECK(bind(ls, (struct sockaddr *)&la, sizeof(la)) == 0);
	CHECK(listen(ls, 4) == 0);
	uint32_t lo = htonl(INADDR_LOOPBACK);

	/* 1. a RST from nobody does not close the listener */
	int before = zero_peers();
	forge(0, 0, lo, PORT, 0, 0, 0x04);
	forge(0, 0, lo, PORT, 0, 0, 0x02);               /* 2. a SYN from nobody */
	CHECK(zero_peers() == before);
	int c = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(c >= 0);
	CHECK(connect(c, (struct sockaddr *)&la, sizeof(la)) == 0);   /* still listening */
	int a = accept(ls, NULL, NULL);
	CHECK(a >= 0);

	/* 3. a guessed RST to the live connection: ignored */
	struct sockaddr_in ca;
	socklen_t cl = sizeof(ca);
	CHECK(getsockname(c, (struct sockaddr *)&ca, &cl) == 0);
	forge(lo, ntohs(ca.sin_port), lo, PORT, 0x12345678u, 0, 0x04);
	forge(lo, PORT, lo, ntohs(ca.sin_port), 0x9abcdef0u, 0, 0x04);
	CHECK(write(c, "x", 1) == 1);
	CHECK(readable(a, 1000));
	char ch;
	CHECK(read(a, &ch, 1) == 1 && ch == 'x');
	CHECK(write(a, "y", 1) == 1);
	CHECK(readable(c, 1000));
	CHECK(read(c, &ch, 1) == 1 && ch == 'y');

	/* 4. a real reset still resets: a port nobody listens on refuses at once */
	int r = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in na = lo_addr(PORT + 1);
	CHECK(r >= 0);
	errno = 0;
	CHECK(connect(r, (struct sockaddr *)&na, sizeof(na)) < 0 && errno == ECONNREFUSED);
	close(r);
	close(c);
	close(a);
	close(ls);
	puts("OK");
	return 0;
}
