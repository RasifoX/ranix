/* A datagram cut short says so, as on Linux:
 * - recvmsg into a smaller buffer returns what fit and sets MSG_TRUNC in
 *   msg_flags; one that fits leaves msg_flags 0;
 * - recv/recvfrom/recvmsg with the MSG_TRUNC flag return the datagram's real
 *   length (the buffer still gets only what fits), also with a zero-length
 *   buffer, the usual way to drop a datagram and learn its size;
 * - recvmsg with no iovec on UDP takes one datagram and reports MSG_TRUNC;
 * - the same on a unix datagram socketpair;
 * - a TCP read that leaves bytes queued is not a truncation (msg_flags 0).
 * Prints OK, or FAIL lines. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int fails;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static char pat[256];

static long rmsg(int fd, void *buf, size_t len, int niov, int flags, int *mflags)
{
	struct iovec v[2] = { { buf, len / 2 }, { (char *)buf + len / 2, len - len / 2 } };
	struct msghdr m;
	memset(&m, 0, sizeof(m));
	m.msg_iov = niov ? v : NULL;
	m.msg_iovlen = (size_t)niov;
	m.msg_flags = 0x7fff0000;   /* the kernel must overwrite it */
	long r = recvmsg(fd, &m, flags);
	*mflags = m.msg_flags;
	return r;
}

static void dgram_cases(const char *k, int a, int b)
{
	char buf[256];
	int mf;
	long r;

	send(a, pat, 100, 0);
	memset(buf, 0, sizeof(buf));
	r = rmsg(b, buf, 10, 2, 0, &mf);
	CHECK(r == 10 && (mf & MSG_TRUNC) && !memcmp(buf, pat, 10),
	      "%s recvmsg 100 into 10: rc %ld msg_flags %#x", k, r, mf);

	send(a, pat, 100, 0);
	r = rmsg(b, buf, 200, 2, 0, &mf);
	CHECK(r == 100 && mf == 0, "%s recvmsg 100 into 200: rc %ld msg_flags %#x", k, r, mf);

	send(a, pat, 100, 0);
	memset(buf, 0, sizeof(buf));
	r = recv(b, buf, 10, MSG_TRUNC);
	CHECK(r == 100 && !memcmp(buf, pat, 10) && buf[10] == 0,
	      "%s recv 100 into 10 with MSG_TRUNC: rc %ld", k, r);

	send(a, pat, 100, 0);
	struct sockaddr_storage ss;
	socklen_t sl = sizeof(ss);
	r = recvfrom(b, buf, 10, MSG_TRUNC, (struct sockaddr *)&ss, &sl);
	CHECK(r == 100, "%s recvfrom 100 into 10 with MSG_TRUNC: rc %ld", k, r);

	send(a, pat, 50, 0);
	r = rmsg(b, buf, 10, 2, MSG_TRUNC, &mf);
	CHECK(r == 50 && (mf & MSG_TRUNC), "%s recvmsg 50 into 10 with MSG_TRUNC: rc %ld msg_flags %#x", k, r, mf);

	send(a, pat, 30, 0);
	r = recv(b, NULL, 0, MSG_TRUNC);
	CHECK(r == 30, "%s recv(NULL, 0, MSG_TRUNC) of 30: rc %ld", k, r);
	r = recv(b, buf, sizeof(buf), MSG_DONTWAIT);
	CHECK(r == -1 && errno == EAGAIN, "%s that datagram was not taken: rc %ld", k, r);

	send(a, pat, 20, 0);
	r = rmsg(b, buf, 200, 2, MSG_TRUNC, &mf);
	CHECK(r == 20 && mf == 0, "%s recvmsg 20 into 200 with MSG_TRUNC: rc %ld msg_flags %#x", k, r, mf);
}

int main(void)
{
	for (int i = 0; i < (int)sizeof(pat); i++) pat[i] = (char)('A' + i % 26);

	/* UDP over loopback, connected both ways */
	int a = socket(AF_INET, SOCK_DGRAM, 0), b = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in sa, sb;
	socklen_t l = sizeof(sa);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sb = sa;
	bind(a, (struct sockaddr *)&sa, sizeof(sa));
	bind(b, (struct sockaddr *)&sb, sizeof(sb));
	getsockname(a, (struct sockaddr *)&sa, &l);
	l = sizeof(sb);
	getsockname(b, (struct sockaddr *)&sb, &l);
	connect(a, (struct sockaddr *)&sb, sizeof(sb));
	connect(b, (struct sockaddr *)&sa, sizeof(sa));
	dgram_cases("udp", a, b);

	/* UDP recvmsg without an iovec still takes a datagram (Linux) */
	char buf[256];
	int mf;
	send(a, pat, 40, 0);
	long r = rmsg(b, buf, 0, 0, 0, &mf);
	CHECK(r == 0 && (mf & MSG_TRUNC), "udp recvmsg no iovec of 40: rc %ld msg_flags %#x", r, mf);
	send(a, pat, 40, 0);
	r = rmsg(b, buf, 0, 0, MSG_TRUNC, &mf);
	CHECK(r == 40 && (mf & MSG_TRUNC), "udp recvmsg no iovec, MSG_TRUNC, of 40: rc %ld msg_flags %#x", r, mf);
	r = recv(b, buf, sizeof(buf), MSG_DONTWAIT);
	CHECK(r == -1 && errno == EAGAIN, "udp queue not empty after the no-iovec reads: rc %ld", r);
	close(a);
	close(b);

	int sv[2];
	socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
	dgram_cases("unix", sv[0], sv[1]);
	close(sv[0]);
	close(sv[1]);

	/* TCP: bytes left queued are not a truncation */
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	bind(ls, (struct sockaddr *)&sa, sizeof(sa));
	l = sizeof(sa);
	getsockname(ls, (struct sockaddr *)&sa, &l);
	listen(ls, 1);
	int c = socket(AF_INET, SOCK_STREAM, 0);
	connect(c, (struct sockaddr *)&sa, sizeof(sa));
	int s = accept(ls, NULL, NULL);
	send(c, pat, 100, 0);
	usleep(100000);
	r = rmsg(s, buf, 10, 2, 0, &mf);
	CHECK(r == 10 && mf == 0, "tcp recvmsg 10 of 100: rc %ld msg_flags %#x", r, mf);
	close(c);
	close(s);
	close(ls);

	if (fails) return 1;
	printf("OK\n");
	return 0;
}
