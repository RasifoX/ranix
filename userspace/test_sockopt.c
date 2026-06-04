/* setsockopt/getsockopt round-trips wired to real behavior:
 *  - SO_REUSEADDR / SO_RCVBUF / TCP_NODELAY get back what was set
 *  - SO_REUSEADDR lets a second socket bind a port already bound
 *  - without SO_REUSEADDR the second bind fails with EADDRINUSE
 *  - SO_RCVTIMEO makes a blocking recv() time out with EAGAIN */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/time.h>

#define PORT 12357

static int getint(int fd, int lv, int opt)
{
	int v = -1; socklen_t l = sizeof(v);
	if (getsockopt(fd, lv, opt, &v, &l) < 0) return -1;
	return v;
}

int main(void)
{
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) { puts("FAIL socket"); return 1; }

	int one = 1;
	if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) { puts("FAIL set reuse"); return 1; }
	if (getint(s, SOL_SOCKET, SO_REUSEADDR) != 1) { puts("FAIL get reuse"); return 1; }

	int rb = 32768;
	if (setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rb, sizeof(rb)) < 0) { puts("FAIL set rcvbuf"); return 1; }
	if (getint(s, SOL_SOCKET, SO_RCVBUF) != 32768) { puts("FAIL get rcvbuf"); return 1; }

	if (setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) { puts("FAIL set nodelay"); return 1; }
	if (getint(s, IPPROTO_TCP, TCP_NODELAY) != 1) { puts("FAIL get nodelay"); return 1; }

	/* SO_REUSEADDR rebind: bind s, then a second reuse socket to the same port */
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port   = htons(PORT);
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) { puts("FAIL bind1"); return 1; }

	int s2 = socket(AF_INET, SOCK_STREAM, 0);
	setsockopt(s2, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(s2, (struct sockaddr *)&a, sizeof(a)) < 0) { puts("FAIL reuse bind"); return 1; }
	close(s2);

	/* without SO_REUSEADDR the same port must be refused */
	int s3 = socket(AF_INET, SOCK_STREAM, 0);
	errno = 0;
	if (bind(s3, (struct sockaddr *)&a, sizeof(a)) == 0 || errno != EADDRINUSE) {
		puts("FAIL noreuse bind"); return 1;
	}
	close(s3);

	/* SO_RCVTIMEO: a recv on an empty UDP socket must return EAGAIN promptly */
	int u = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in ua;
	memset(&ua, 0, sizeof(ua));
	ua.sin_family = AF_INET;
	ua.sin_port   = htons(PORT + 1);
	ua.sin_addr.s_addr = htonl(INADDR_ANY);
	bind(u, (struct sockaddr *)&ua, sizeof(ua));

	struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };   /* 200ms */
	if (setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) { puts("FAIL set rcvtimeo"); return 1; }

	struct timeval t0, t1;
	gettimeofday(&t0, NULL);
	char b[8];
	errno = 0;
	int n = (int)recv(u, b, sizeof(b), 0);
	gettimeofday(&t1, NULL);
	close(u);
	close(s);
	if (n != -1 || errno != EAGAIN) { puts("FAIL rcvtimeo errno"); return 1; }
	/* must have actually blocked for a while, but not forever — exact duration
	 * depends on the platform timer rate, so the window is wide */
	long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_usec - t0.tv_usec) / 1000;
	if (ms < 40 || ms > 3000) { printf("FAIL rcvtimeo timing ms=%ld\n", ms); return 1; }

	puts("OK");
	return 0;
}
