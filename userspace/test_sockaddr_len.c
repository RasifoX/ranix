/* connect/sendto/sendmsg read exactly the address length the caller gave, the
 * way Linux's move_addr_to_kernel does, and refuse a length that cannot hold
 * the family's address:
 * - addrlen > 128 (sockaddr_storage) is EINVAL for connect and sendto;
 * - an AF_INET address shorter than 16 bytes is EINVAL (connect, sendto, and a
 *   sendto with an address but addrlen 0);
 * - an AF_INET6 address of 24 bytes (RFC 2133, no scope id) is accepted when
 *   it ends at the last byte of a page with nothing mapped after it: reading a
 *   full 28-byte sockaddr_in6 there would fault (EFAULT);
 * - sendmsg treats msg_namelen 0 as "no address" (goes to the connected peer)
 *   and clamps a msg_namelen above 128 instead of refusing it.
 * Prints OK, or FAIL with the first case that differs. */
#define _GNU_SOURCE
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#define PORT_A 12391
#define PORT_B 12392
#define PORT_6 12393

static int fails;

static void expect(const char *what, long got, long want_rc, int want_errno)
{
	int e = errno;
	if (want_rc < 0 ? (got >= 0 || e != want_errno) : got != want_rc) {
		printf("FAIL: %s: rc %ld errno %d (want rc %ld errno %d)\n",
		       what, got, got < 0 ? e : 0, want_rc, want_rc < 0 ? want_errno : 0);
		fails++;
	}
}

static int udp_bound(int fam, int port)
{
	int s = socket(fam, SOCK_DGRAM, 0);
	if (s < 0) return -1;
	if (fam == AF_INET) {
		struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { close(s); return -1; }
	} else {
		struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_port = htons(port) };
		a.sin6_addr = in6addr_loopback;
		if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { close(s); return -1; }
	}
	return s;
}

/* 1 when a datagram arrives on s within a second (and drains it) */
static int got_one(int s)
{
	struct pollfd p = { .fd = s, .events = POLLIN };
	char b[16];
	if (poll(&p, 1, 1000) != 1) return 0;
	return recv(s, b, sizeof(b), MSG_DONTWAIT) > 0;
}

static struct sockaddr_in lo4(int port)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	return a;
}

static void v4_cases(void)
{
	int ra = udp_bound(AF_INET, PORT_A), rb = udp_bound(AF_INET, PORT_B);
	if (ra < 0 || rb < 0) { puts("FAIL: bind 127.0.0.1"); fails++; return; }
	struct sockaddr_in a = lo4(PORT_A), b = lo4(PORT_B);
	unsigned char big[200];
	memset(big, 0, sizeof(big));
	memcpy(big, &a, sizeof(a));

	int u = socket(AF_INET, SOCK_DGRAM, 0);
	expect("udp4 connect addrlen 15", connect(u, (struct sockaddr *)&a, 15), -1, EINVAL);
	expect("udp4 connect addrlen 129", connect(u, (struct sockaddr *)big, 129), -1, EINVAL);
	expect("udp4 sendto addrlen 15", sendto(u, "x", 1, 0, (struct sockaddr *)&a, 15), -1, EINVAL);
	expect("udp4 sendto addrlen 0", sendto(u, "x", 1, 0, (struct sockaddr *)&a, 0), -1, EINVAL);
	expect("udp4 sendto addrlen 129", sendto(u, "x", 1, 0, (struct sockaddr *)big, 129), -1, EINVAL);
	expect("udp4 sendto addrlen 128", sendto(u, "x", 1, 0, (struct sockaddr *)big, 128), 1, 0);
	if (!got_one(ra)) { puts("FAIL: udp4 sendto addrlen 128 not delivered"); fails++; }
	close(u);

	int t = socket(AF_INET, SOCK_STREAM, 0);
	expect("tcp4 connect addrlen 15", connect(t, (struct sockaddr *)&a, 15), -1, EINVAL);
	close(t);

	/* sendmsg on a socket connected to A: namelen 0 ignores the name (B) */
	u = socket(AF_INET, SOCK_DGRAM, 0);
	expect("udp4 connect A", connect(u, (struct sockaddr *)&a, sizeof(a)), 0, 0);
	struct iovec v = { "y", 1 };
	struct msghdr m = { .msg_name = &b, .msg_namelen = 0, .msg_iov = &v, .msg_iovlen = 1 };
	expect("sendmsg namelen 0", sendmsg(u, &m, 0), 1, 0);
	if (!got_one(ra)) { puts("FAIL: sendmsg namelen 0 did not go to the connected peer"); fails++; }
	if (got_one(rb)) { puts("FAIL: sendmsg namelen 0 went to msg_name"); fails++; }
	/* a namelen above 128 is clamped, not refused */
	unsigned char bigb[200];
	memset(bigb, 0, sizeof(bigb));
	memcpy(bigb, &b, sizeof(b));
	m.msg_name = bigb; m.msg_namelen = sizeof(bigb);
	expect("sendmsg namelen 200", sendmsg(u, &m, 0), 1, 0);
	if (!got_one(rb)) { puts("FAIL: sendmsg namelen 200 not delivered to msg_name"); fails++; }
	close(u); close(ra); close(rb);
}

/* the 24 bytes of an RFC 2133 sockaddr_in6 end at the page's last byte */
static void v6_cases(void)
{
	long pg = sysconf(_SC_PAGESIZE);
	unsigned char *map = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map == MAP_FAILED || mprotect(map + pg, pg, PROT_NONE)) { puts("FAIL: mmap"); fails++; return; }
	int r = udp_bound(AF_INET6, PORT_6);
	if (r < 0) { puts("FAIL: bind ::1"); fails++; return; }
	struct sockaddr_in6 full = { .sin6_family = AF_INET6, .sin6_port = htons(PORT_6) };
	full.sin6_addr = in6addr_loopback;
	unsigned char *edge = map + pg - 24;
	memcpy(edge, &full, 24);

	int u = socket(AF_INET6, SOCK_DGRAM, 0);
	expect("udp6 sendto 24 bytes at page end", sendto(u, "z", 1, 0, (struct sockaddr *)edge, 24), 1, 0);
	if (!got_one(r)) { puts("FAIL: udp6 sendto 24 bytes not delivered"); fails++; }
	expect("udp6 sendto addrlen 23", sendto(u, "z", 1, 0, (struct sockaddr *)&full, 23), -1, EINVAL);
	expect("udp6 connect addrlen 23", connect(u, (struct sockaddr *)&full, 23), -1, EINVAL);
	expect("udp6 connect 24 bytes at page end", connect(u, (struct sockaddr *)edge, 24), 0, 0);
	expect("udp6 send after connect", send(u, "z", 1, 0), 1, 0);
	if (!got_one(r)) { puts("FAIL: udp6 send after a 24-byte connect not delivered"); fails++; }
	close(u); close(r);
	munmap(map, 2 * pg);
}

int main(void)
{
	v4_cases();
	v6_cases();
	if (!fails) puts("OK");
	return fails != 0;
}
