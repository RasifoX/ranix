/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* recvmsg on TCP and UDP scatters the data over every iovec in order, as on
 * Linux (and as AF_UNIX already did), instead of filling only the first one:
 * - UDP: a 10-byte datagram into iovecs of 3, 0, 4 and 16 bytes reads 10, split
 *   "abc" | "" | "defg" | "hij", with the sender's address in msg_name;
 * - UDP: a datagram longer than all iovecs fills them (3 + 4) and the rest is
 *   dropped: the next recvmsg gets the next datagram;
 * - TCP: 10 queued bytes into iovecs of 4 and 16 read 10, "abcd" | "efghij".
 * Prints OK, or FAIL with the first case that differs. */
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define PORT_U 12394
#define PORT_T 12395

static int fails;
#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); puts(""); fails++; } while (0)

static struct sockaddr_in lo(int port)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	return a;
}

static int readable(int s)
{
	struct pollfd p = { .fd = s, .events = POLLIN };
	return poll(&p, 1, 2000) == 1;
}

static void udp(void)
{
	struct sockaddr_in a = lo(PORT_U);
	int r = socket(AF_INET, SOCK_DGRAM, 0), w = socket(AF_INET, SOCK_DGRAM, 0);
	if (r < 0 || w < 0 || bind(r, (struct sockaddr *)&a, sizeof(a))) { FAIL("udp setup"); return; }
	struct sockaddr_in wa = lo(PORT_U + 10);
	if (bind(w, (struct sockaddr *)&wa, sizeof(wa))) { FAIL("udp bind sender"); return; }

	char b0[3], b1[1], b2[4], b3[16];
	memset(b3, 0, sizeof(b3));
	struct iovec v[4] = { { b0, 3 }, { b1, 0 }, { b2, 4 }, { b3, 16 } };
	struct sockaddr_in from;
	struct msghdr m = { .msg_name = &from, .msg_namelen = sizeof(from), .msg_iov = v, .msg_iovlen = 4 };
	sendto(w, "abcdefghij", 10, 0, (struct sockaddr *)&a, sizeof(a));
	if (!readable(r)) { FAIL("udp nothing arrived"); return; }
	long n = recvmsg(r, &m, 0);
	if (n != 10) FAIL("udp recvmsg returned %ld, want 10", n);
	else if (memcmp(b0, "abc", 3) || memcmp(b2, "defg", 4) || memcmp(b3, "hij", 3))
		FAIL("udp pieces '%.3s' '%.4s' '%.3s'", b0, b2, b3);
	if (m.msg_namelen != sizeof(from) || from.sin_port != htons(PORT_U + 10))
		FAIL("udp msg_name port %d len %u", ntohs(from.sin_port), m.msg_namelen);

	/* longer than every iovec together: 7 bytes, the rest of it is gone */
	sendto(w, "0123456789", 10, 0, (struct sockaddr *)&a, sizeof(a));
	sendto(w, "next", 4, 0, (struct sockaddr *)&a, sizeof(a));
	struct iovec t[2] = { { b0, 3 }, { b2, 4 } };
	struct msghdr mt = { .msg_iov = t, .msg_iovlen = 2 };
	if (!readable(r)) { FAIL("udp second datagram missing"); return; }
	n = recvmsg(r, &mt, 0);
	if (n != 7 || memcmp(b0, "012", 3) || memcmp(b2, "3456", 4)) FAIL("udp truncated read %ld", n);
	char nb[16];
	if (!readable(r)) { FAIL("udp third datagram missing"); return; }
	n = recv(r, nb, sizeof(nb), 0);
	if (n != 4 || memcmp(nb, "next", 4)) FAIL("udp after a truncated datagram read %ld", n);
	close(r); close(w);
}

static void tcp(void)
{
	struct sockaddr_in a = lo(PORT_T);
	int l = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof(a)) || listen(l, 1)) { FAIL("tcp listen"); return; }
	int c = socket(AF_INET, SOCK_STREAM, 0);
	if (connect(c, (struct sockaddr *)&a, sizeof(a))) { FAIL("tcp connect"); return; }
	int s = accept(l, NULL, NULL);
	if (s < 0) { FAIL("tcp accept"); return; }
	if (write(c, "abcdefghij", 10) != 10) { FAIL("tcp write"); return; }
	usleep(200000);   /* one segment on loopback; let it all arrive */
	char b0[4], b1[16];
	memset(b1, 0, sizeof(b1));
	struct iovec v[2] = { { b0, 4 }, { b1, 16 } };
	struct msghdr m = { .msg_iov = v, .msg_iovlen = 2 };
	if (!readable(s)) { FAIL("tcp nothing arrived"); return; }
	long n = recvmsg(s, &m, 0);
	if (n != 10) FAIL("tcp recvmsg returned %ld, want 10", n);
	else if (memcmp(b0, "abcd", 4) || memcmp(b1, "efghij", 6)) FAIL("tcp pieces '%.4s' '%.6s'", b0, b1);
	close(s); close(c); close(l);
}

int main(void)
{
	udp();
	tcp();
	if (!fails) puts("OK");
	return fails != 0;
}
