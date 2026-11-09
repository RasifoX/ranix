/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* splice from a pipe into a connected UDP socket (IPv4 and IPv6) sends the
 * data as a datagram (LTP setsockopt11); sendfile into one too. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int bad;

static void one(int fam)
{
	struct sockaddr_storage ss;
	memset(&ss, 0, sizeof(ss));
	socklen_t len;
	if (fam == AF_INET) {
		struct sockaddr_in *a = (struct sockaddr_in *)&ss;
		a->sin_family = AF_INET; a->sin_addr.s_addr = htonl(INADDR_LOOPBACK); len = sizeof(*a);
	} else {
		struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
		a->sin6_family = AF_INET6; a->sin6_addr = in6addr_loopback; len = sizeof(*a);
	}
	int rx = socket(fam, SOCK_DGRAM, 0), tx = socket(fam, SOCK_DGRAM, 0);
	if (fam == AF_INET6 && (rx < 0 || bind(rx, (struct sockaddr *)&ss, len))) {
		printf("no ::1 here (errno %d): IPv6 case skipped\n", errno); close(rx); close(tx); return;
	}
	if ((fam == AF_INET && bind(rx, (struct sockaddr *)&ss, len)) || getsockname(rx, (struct sockaddr *)&ss, &len) ||
	    connect(tx, (struct sockaddr *)&ss, len)) { printf("af %d: setup failed\n", fam); bad++; return; }
	struct timeval tv = { 2, 0 };
	setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	int p[2];
	pipe(p);
	write(p[1], "spliced", 7);
	long n = splice(p[0], NULL, tx, NULL, 7, 0);   /* SPLICE_F_MORE would cork it */
	char b[32] = { 0 };
	if (n != 7) { printf("af %d: splice into udp: %ld\n", fam, n); bad++; }
	else if (recv(rx, b, sizeof(b), 0) != 7 || memcmp(b, "spliced", 7)) { printf("af %d: received '%s'\n", fam, b); bad++; }
	int f = open("/tmp/spliceudp", O_CREAT | O_RDWR | O_TRUNC, 0644);
	write(f, "sentfile", 8);
	off_t off = 0;
	n = sendfile(tx, f, &off, 8);
	memset(b, 0, sizeof(b));
	if (n != 8) { printf("af %d: sendfile into udp: %ld\n", fam, n); bad++; }
	else if (recv(rx, b, sizeof(b), 0) != 8 || memcmp(b, "sentfile", 8)) { printf("af %d: received '%s'\n", fam, b); bad++; }
	close(f); unlink("/tmp/spliceudp");
	close(p[0]); close(p[1]); close(rx); close(tx);
}

int main(void)
{
	one(AF_INET);
	one(AF_INET6);
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
