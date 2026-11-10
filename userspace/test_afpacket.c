/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* AF_PACKET (LTP setsockopt06, 07, 09 open one): a SOCK_DGRAM socket for
 * ETH_P_IP on lo sees a UDP datagram sent to 127.0.0.1, with a sockaddr_ll
 * naming lo; a SOCK_RAW ETH_P_ALL socket sends a whole frame on lo and sees it
 * come in. Without CAP_NET_RAW, socket() is EPERM. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static int bad;

/* the first incoming (not PACKET_OUTGOING) frame holding needle, or -1 */
static int find(int s, const char *needle, struct sockaddr_ll *from)
{
	static char b[2048];
	for (int i = 0; i < 20; i++) {
		socklen_t fl = sizeof(*from);
		ssize_t n = recvfrom(s, b, sizeof(b), 0, (struct sockaddr *)from, &fl);
		if (n < 0) return -1;
		if (from->sll_pkttype == PACKET_OUTGOING) continue;
		if (memmem(b, (size_t)n, needle, strlen(needle))) return (int)n;
	}
	return -1;
}

int main(void)
{
	int lo = (int)if_nametoindex("lo");
	struct timeval tv = { 2, 0 };
	int d = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
	if (d < 0) { printf("socket(AF_PACKET, SOCK_DGRAM): errno %d\n", errno); return 1; }
	setsockopt(d, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	struct sockaddr_ll b = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_IP), .sll_ifindex = lo };
	if (bind(d, (struct sockaddr *)&b, sizeof(b))) { printf("bind to lo: errno %d\n", errno); bad++; }
	struct sockaddr_ll nm; socklen_t nl = sizeof(nm);
	if (getsockname(d, (struct sockaddr *)&nm, &nl) || nm.sll_family != AF_PACKET || nm.sll_ifindex != lo) { puts("getsockname"); bad++; }
	int u = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(9), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	sendto(u, "packet-probe", 12, 0, (struct sockaddr *)&to, sizeof(to));
	struct sockaddr_ll from;
	if (find(d, "packet-probe", &from) < 0) { puts("SOCK_DGRAM: the UDP datagram was not seen"); bad++; }
	else if (from.sll_family != AF_PACKET || from.sll_protocol != htons(ETH_P_IP) || from.sll_ifindex != lo) {
		printf("sockaddr_ll: family %d proto 0x%x ifindex %d\n", from.sll_family, ntohs(from.sll_protocol), from.sll_ifindex); bad++;
	}
	int r = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
	setsockopt(r, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	struct sockaddr_ll rb = { .sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_ALL), .sll_ifindex = lo };
	bind(r, (struct sockaddr *)&rb, sizeof(rb));
	unsigned char frame[64] = { 0 };   /* dst, src all zero (lo); experimental ethertype */
	frame[12] = 0x88; frame[13] = 0xb5;
	memcpy(frame + 14, "raw-frame-probe", 15);
	struct sockaddr_ll ta = { .sll_family = AF_PACKET, .sll_ifindex = lo, .sll_halen = 6 };
	if (sendto(r, frame, sizeof(frame), 0, (struct sockaddr *)&ta, sizeof(ta)) != sizeof(frame)) { printf("SOCK_RAW send: errno %d\n", errno); bad++; }
	else if (find(r, "raw-frame-probe", &from) < 0) { puts("SOCK_RAW: the frame was not seen"); bad++; }
	pid_t c = fork();
	if (c == 0) {
		if (setuid(65534)) _exit(2);
		errno = 0;
		_exit(socket(AF_PACKET, SOCK_RAW, 0) == -1 && errno == EPERM ? 0 : 1);
	}
	int st;
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) { puts("unprivileged AF_PACKET socket: not EPERM"); bad++; }
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
