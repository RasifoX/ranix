/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* How much is waiting, as Linux answers it: FIONREAD (= TIOCINQ = SIOCINQ) and
 * TIOCOUTQ (= SIOCOUTQ). A test harness that waits for a terminal to settle
 * asks a pty slave how much input is queued; it got ENOTTY. Checked against
 * Linux 6.8 (the same program, same numbers):
 *   pty slave: committed input only (a canonical line counts once its newline
 *     is in, raw input at once); master: what the slave wrote, echo included;
 *     TIOCOUTQ 0 on both ends (a write lands in the peer's input at once);
 *     TCFLSH TCIFLUSH empties that end's input, TCOFLUSH leaves what the peer
 *     already holds, any other argument EINVAL.
 *   console: FIONREAD/TIOCOUTQ answer, TCFLSH works.
 *   pipe: FIONREAD on either end, TIOCOUTQ ENOTTY.
 *   sockets: a stream sums its queue, a datagram is the next one's size, a
 *     seqpacket sums; a listener: SIOCINQ EINVAL (TCP's SIOCOUTQ too, a unix
 *     listener's is 0); TCP SIOCOUTQ is what the peer has not acked;
 *     SIOCOUTQNSD what is not yet sent; a unix sender's SIOCOUTQ is more than
 *     0 while the peer has not read, 0 once it has.
 *   a regular file: its size less the offset; /dev/null: ENOTTY.
 * Prints FAIL lines, then OK or FAIL. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/sockios.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ioctl(fd, req, &int): the value, or -1000 - errno */
static int qv(int fd, unsigned long req)
{
	int v = -7;
	if (ioctl(fd, req, &v) != 0) return -1000 - errno;
	return v;
}
#define WANT(what, got, want) do { int g_ = (got); CHECK(g_ == (want), "%s: got %d, want %d%s", what, g_, (int)(want), g_ <= -1000 ? " (an errno: -1000-e)" : ""); } while (0)
#define E(e) (-1000 - (e))

static void settle(void) { usleep(50000); }   /* Linux hands pty input over in a work item */

/* wait up to 2 s for ioctl(fd, req) to read want (TCP acks over loopback) */
static int wait_for(int fd, unsigned long req, int want)
{
	int v = 0;
	for (int i = 0; i < 200; i++) {
		v = qv(fd, req);
		if (v == want) break;
		usleep(10000);
	}
	return v;
}

static void pty(void)
{
	int m = posix_openpt(O_RDWR | O_NOCTTY);
	if (m < 0 || grantpt(m) || unlockpt(m)) { CHECK(0, "posix_openpt: %s", strerror(errno)); return; }
	int s = open(ptsname(m), O_RDWR | O_NOCTTY);
	if (s < 0) { CHECK(0, "open slave: %s", strerror(errno)); return; }
	char b[64];

	WANT("pty slave FIONREAD empty", qv(s, FIONREAD), 0);
	WANT("pty slave TIOCINQ empty", qv(s, TIOCINQ), 0);
	WANT("pty slave TIOCOUTQ", qv(s, TIOCOUTQ), 0);
	WANT("pty master FIONREAD empty", qv(m, FIONREAD), 0);
	WANT("pty master TIOCOUTQ", qv(m, TIOCOUTQ), 0);

	CHECK(write(m, "abc", 3) == 3, "master write"); settle();
	WANT("pty slave FIONREAD, canonical line not ended", qv(s, FIONREAD), 0);
	WANT("pty master FIONREAD, the echo of abc", qv(m, FIONREAD), 3);
	WANT("pty master TIOCOUTQ after a write", qv(m, TIOCOUTQ), 0);
	CHECK(write(m, "\n", 1) == 1, "master write"); settle();
	WANT("pty slave FIONREAD, one line", qv(s, FIONREAD), 4);
	WANT("pty master FIONREAD, echo abc\\r\\n", qv(m, FIONREAD), 5);
	CHECK(write(m, "de\nfg\n", 6) == 6, "master write"); settle();
	WANT("pty slave FIONREAD, three lines", qv(s, FIONREAD), 10);
	CHECK(read(s, b, sizeof b) == 4, "canonical read of one line");
	WANT("pty slave FIONREAD after reading a line", qv(s, FIONREAD), 6);

	CHECK(tcflush(s, TCIFLUSH) == 0, "tcflush(slave, TCIFLUSH): %s", strerror(errno));
	WANT("pty slave FIONREAD after TCIFLUSH", qv(s, FIONREAD), 0);
	WANT("pty master FIONREAD before its TCIFLUSH", qv(m, FIONREAD), 13);
	CHECK(tcflush(m, TCIFLUSH) == 0, "tcflush(master, TCIFLUSH): %s", strerror(errno));
	WANT("pty master FIONREAD after TCIFLUSH", qv(m, FIONREAD), 0);
	errno = 0;
	CHECK(ioctl(s, TCFLSH, 7) == -1 && errno == EINVAL, "TCFLSH 7: want EINVAL, got %s", strerror(errno));

	struct termios t;
	CHECK(tcgetattr(s, &t) == 0, "tcgetattr");
	cfmakeraw(&t);
	CHECK(tcsetattr(s, TCSANOW, &t) == 0, "tcsetattr raw");
	CHECK(write(m, "xyz", 3) == 3, "master write"); settle();
	WANT("pty slave FIONREAD raw", qv(s, FIONREAD), 3);
	CHECK(write(s, "hello\n", 6) == 6, "slave write"); settle();
	WANT("pty master FIONREAD after a raw slave write", qv(m, FIONREAD), 6);
	WANT("pty slave TIOCOUTQ after a write", qv(s, TIOCOUTQ), 0);
	CHECK(tcflush(s, TCOFLUSH) == 0, "tcflush(slave, TCOFLUSH)");
	WANT("pty master FIONREAD after the slave's TCOFLUSH", qv(m, FIONREAD), 6);
	CHECK(write(m, "123", 3) == 3, "master write"); settle();
	CHECK(tcflush(m, TCOFLUSH) == 0, "tcflush(master, TCOFLUSH)");
	WANT("pty slave FIONREAD after the master's TCOFLUSH", qv(s, FIONREAD), 6);
	CHECK(tcflush(s, TCIOFLUSH) == 0, "tcflush(slave, TCIOFLUSH)");
	WANT("pty slave FIONREAD after TCIOFLUSH", qv(s, FIONREAD), 0);
	close(s); close(m);
}

static void console(void)
{
	int c = open("/dev/console", O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (c < 0) { printf("console: not opened (%s), skipped\n", strerror(errno)); return; }
	int v = qv(c, FIONREAD);
	CHECK(v >= 0, "console FIONREAD: %d (want >= 0)", v);
	WANT("console TIOCOUTQ", qv(c, TIOCOUTQ), 0);
	CHECK(tcflush(c, TCIFLUSH) == 0, "tcflush(console, TCIFLUSH): %s", strerror(errno));
	WANT("console FIONREAD after TCIFLUSH", qv(c, FIONREAD), 0);
	close(c);
}

static void pipes(void)
{
	int p[2];
	if (pipe(p)) { CHECK(0, "pipe"); return; }
	CHECK(write(p[1], "12345", 5) == 5, "pipe write");
	WANT("pipe read end FIONREAD", qv(p[0], FIONREAD), 5);
	WANT("pipe write end FIONREAD", qv(p[1], FIONREAD), 5);
	WANT("pipe TIOCOUTQ", qv(p[1], TIOCOUTQ), E(ENOTTY));
	close(p[0]); close(p[1]);
}

static void unix_socks(void)
{
	int sv[2];
	char b[16];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { CHECK(0, "socketpair stream"); return; }
	CHECK(write(sv[0], "abc", 3) == 3 && write(sv[0], "de", 2) == 2, "unix stream write");
	WANT("unix stream SIOCINQ", qv(sv[1], SIOCINQ), 5);
	CHECK(qv(sv[0], SIOCOUTQ) > 0, "unix stream sender SIOCOUTQ: %d, want > 0 while unread", qv(sv[0], SIOCOUTQ));
	WANT("unix stream receiver SIOCOUTQ", qv(sv[1], SIOCOUTQ), 0);
	CHECK(read(sv[1], b, 4) == 4, "unix stream read 4");
	WANT("unix stream SIOCINQ after reading 4", qv(sv[1], SIOCINQ), 1);
	CHECK(read(sv[1], b, 4) == 1, "unix stream read the rest");
	WANT("unix stream sender SIOCOUTQ once all is read", qv(sv[0], SIOCOUTQ), 0);
	close(sv[0]); close(sv[1]);

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv)) { CHECK(0, "socketpair dgram"); return; }
	CHECK(write(sv[0], "abc", 3) == 3 && write(sv[0], "defgh", 5) == 5, "unix dgram write");
	WANT("unix dgram SIOCINQ is the next datagram", qv(sv[1], SIOCINQ), 3);
	CHECK(qv(sv[0], SIOCOUTQ) > 0, "unix dgram sender SIOCOUTQ: %d, want > 0 while unread", qv(sv[0], SIOCOUTQ));
	CHECK(read(sv[1], b, sizeof b) == 3, "unix dgram read");
	WANT("unix dgram SIOCINQ, the second datagram", qv(sv[1], SIOCINQ), 5);
	close(sv[0]); close(sv[1]);

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv)) { CHECK(0, "socketpair seqpacket"); return; }
	CHECK(write(sv[0], "abc", 3) == 3 && write(sv[0], "defgh", 5) == 5, "unix seqpacket write");
	WANT("unix seqpacket SIOCINQ sums", qv(sv[1], SIOCINQ), 8);
	close(sv[0]); close(sv[1]);

	int u = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	memcpy(a.sun_path + 1, "fionread-test", 13);
	socklen_t al = (socklen_t)(sizeof(sa_family_t) + 14);
	CHECK(u >= 0 && bind(u, (struct sockaddr *)&a, al) == 0 && listen(u, 1) == 0, "unix listen: %s", strerror(errno));
	WANT("unix listener SIOCINQ", qv(u, SIOCINQ), E(EINVAL));
	WANT("unix listener SIOCOUTQ", qv(u, SIOCOUTQ), 0);
	close(u);
}

static void inet_socks(void)
{
	char b[16];
	int l = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in in = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t sl = sizeof in;
	if (l < 0 || bind(l, (struct sockaddr *)&in, sizeof in) || getsockname(l, (struct sockaddr *)&in, &sl) || listen(l, 1)) {
		CHECK(0, "tcp listen: %s", strerror(errno)); return;
	}
	WANT("tcp listener SIOCINQ", qv(l, SIOCINQ), E(EINVAL));
	WANT("tcp listener SIOCOUTQ", qv(l, SIOCOUTQ), E(EINVAL));
	int c = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(connect(c, (struct sockaddr *)&in, sizeof in) == 0, "tcp connect: %s", strerror(errno));
	int a = accept(l, NULL, NULL);
	CHECK(a >= 0, "tcp accept");
	CHECK(write(c, "abcdefg", 7) == 7, "tcp write");
	WANT("tcp SIOCINQ", wait_for(a, SIOCINQ, 7), 7);
	WANT("tcp SIOCOUTQ once acked", wait_for(c, SIOCOUTQ, 0), 0);
	WANT("tcp SIOCOUTQNSD", qv(c, SIOCOUTQNSD), 0);
	CHECK(read(a, b, 3) == 3, "tcp read 3");
	WANT("tcp SIOCINQ after reading 3", qv(a, SIOCINQ), 4);
	close(a); close(c); close(l);

	int t = socket(AF_INET, SOCK_STREAM, 0);
	WANT("tcp unconnected SIOCINQ", qv(t, SIOCINQ), 0);
	WANT("tcp unconnected SIOCOUTQ", qv(t, SIOCOUTQ), 0);
	close(t);

	int d1 = socket(AF_INET, SOCK_DGRAM, 0), d2 = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in di = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	sl = sizeof di;
	if (d1 < 0 || d2 < 0 || bind(d2, (struct sockaddr *)&di, sizeof di) || getsockname(d2, (struct sockaddr *)&di, &sl)) {
		CHECK(0, "udp bind: %s", strerror(errno)); return;
	}
	WANT("udp SIOCINQ empty", qv(d2, SIOCINQ), 0);
	CHECK(sendto(d1, "abc", 3, 0, (struct sockaddr *)&di, sizeof di) == 3, "udp send");
	CHECK(sendto(d1, "defgh", 5, 0, (struct sockaddr *)&di, sizeof di) == 5, "udp send");
	WANT("udp SIOCINQ is the next datagram", wait_for(d2, SIOCINQ, 3), 3);
	WANT("udp SIOCOUTQ", qv(d1, SIOCOUTQ), 0);
	CHECK(recv(d2, b, sizeof b, 0) == 3, "udp recv");
	WANT("udp SIOCINQ, the second datagram", qv(d2, SIOCINQ), 5);
	close(d1); close(d2);
}

static void files(void)
{
	const char *path = "/tmp/fionread-file";
	int f = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (f < 0) { CHECK(0, "make %s", path); return; }
	CHECK(write(f, "hello world", 11) == 11, "file write");
	WANT("regular file FIONREAD at the end", qv(f, FIONREAD), 0);
	lseek(f, 4, SEEK_SET);
	WANT("regular file FIONREAD = size - offset", qv(f, FIONREAD), 7);
	close(f);
	unlink(path);
	int n = open("/dev/null", O_RDWR);
	WANT("/dev/null FIONREAD", qv(n, FIONREAD), E(ENOTTY));
	close(n);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	pty();
	console();
	pipes();
	unix_socks();
	inet_socks();
	files();
	printf("%s\n", fails ? "FAIL" : "OK");
	return fails ? 1 : 0;
}
