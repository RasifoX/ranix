/* IPv4-mapped IPv6 (::ffff:a.b.c.d) on AF_INET6 sockets: Java opens every
 * socket as IPv6, so Gradle's daemon (bound to 127.0.0.1) and its client met
 * only through mapped addresses and never connected. TCP both ways, a dual
 * stack "::" listener taking an IPv4 client, IPV6_V6ONLY refusing it, UDP. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

static struct sockaddr_in6 m6(const char *a, int port)
{
	struct sockaddr_in6 s;
	memset(&s, 0, sizeof s);
	s.sin6_family = AF_INET6;
	s.sin6_port = htons(port);
	inet_pton(AF_INET6, a, &s.sin6_addr);
	return s;
}

static int is_mapped_lo(const struct sockaddr_in6 *s)
{
	struct in6_addr w;
	inet_pton(AF_INET6, "::ffff:127.0.0.1", &w);
	return s->sin6_family == AF_INET6 && !memcmp(&s->sin6_addr, &w, sizeof w);
}

int main(void)
{
	/* 1: mapped listener, mapped client */
	int l = socket(AF_INET6, SOCK_STREAM, 0);
	struct sockaddr_in6 a = m6("::ffff:127.0.0.1", 0), got;
	socklen_t gl = sizeof got;
	if (bind(l, (void *)&a, sizeof a) || listen(l, 4)) { printf("FAIL bind mapped %d\n", errno); return 0; }
	if (getsockname(l, (void *)&got, &gl) || !is_mapped_lo(&got)) { printf("FAIL listener name\n"); return 0; }
	int port = ntohs(got.sin6_port);
	int c = socket(AF_INET6, SOCK_STREAM, 0);
	struct sockaddr_in6 to = m6("::ffff:127.0.0.1", port);
	if (connect(c, (void *)&to, sizeof to)) { printf("FAIL connect mapped %d\n", errno); return 0; }
	gl = sizeof got;
	int s = accept(l, (void *)&got, &gl);
	if (s < 0 || !is_mapped_lo(&got)) { printf("FAIL accept mapped\n"); return 0; }
	char b[4] = {0};
	if (write(c, "hey", 3) != 3 || read(s, b, 3) != 3 || strcmp(b, "hey")) { printf("FAIL data\n"); return 0; }
	gl = sizeof got;
	if (getpeername(c, (void *)&got, &gl) || !is_mapped_lo(&got) || ntohs(got.sin6_port) != port) { printf("FAIL peername\n"); return 0; }
	close(c); close(s); close(l);

	/* 2: dual-stack "::" listener, IPv4 client */
	l = socket(AF_INET6, SOCK_STREAM, 0);
	a = m6("::", 0);
	gl = sizeof got;
	if (bind(l, (void *)&a, sizeof a) || listen(l, 4) || getsockname(l, (void *)&got, &gl)) { printf("FAIL bind any\n"); return 0; }
	port = ntohs(got.sin6_port);
	c = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in t4 = {0};
	t4.sin_family = AF_INET; t4.sin_port = htons(port); t4.sin_addr.s_addr = htonl(0x7f000001);
	if (connect(c, (void *)&t4, sizeof t4)) { printf("FAIL v4 to dual stack %d\n", errno); return 0; }
	gl = sizeof got;
	s = accept(l, (void *)&got, &gl);
	if (s < 0 || !is_mapped_lo(&got)) { printf("FAIL dual accept\n"); return 0; }
	close(c); close(s); close(l);

	/* 3: IPV6_V6ONLY takes no IPv4 */
	l = socket(AF_INET6, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(l, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof one);
	a = m6("::", 0);
	gl = sizeof got;
	if (bind(l, (void *)&a, sizeof a) || listen(l, 4) || getsockname(l, (void *)&got, &gl)) { printf("FAIL bind v6only\n"); return 0; }
	c = socket(AF_INET, SOCK_STREAM, 0);
	t4.sin_port = got.sin6_port;
	if (connect(c, (void *)&t4, sizeof t4) == 0) { printf("FAIL v6only took ipv4\n"); return 0; }
	close(c); close(l);

	/* 4: UDP through mapped addresses */
	int u = socket(AF_INET6, SOCK_DGRAM, 0), v = socket(AF_INET6, SOCK_DGRAM, 0);
	a = m6("::ffff:127.0.0.1", 0);
	gl = sizeof got;
	if (bind(u, (void *)&a, sizeof a) || getsockname(u, (void *)&got, &gl)) { printf("FAIL udp bind\n"); return 0; }
	to = m6("::ffff:127.0.0.1", ntohs(got.sin6_port));
	if (sendto(v, "dg", 2, 0, (void *)&to, sizeof to) != 2) { printf("FAIL udp send %d\n", errno); return 0; }
	gl = sizeof got;
	memset(b, 0, sizeof b);
	if (recvfrom(u, b, sizeof b, 0, (void *)&got, &gl) != 2 || strcmp(b, "dg") || !is_mapped_lo(&got)) { printf("FAIL udp recv\n"); return 0; }
	printf("OK\n");
	return 0;
}
