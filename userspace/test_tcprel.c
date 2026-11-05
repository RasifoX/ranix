/* TCP that does not leave a peer hanging. Over loopback, with firewall rules in
 * the role of a lost path: half-close, zero-window recovery with the window update
 * lost, data sent just before close, and the RST after retransmits run out.
 * First failure or OK. Needs ~110 s: giving up takes its full backoff. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SOL_RANIX_FILTER 0x52414e58
#define NF_RULE 1
#define MIB (1024 * 1024)

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s errno=%d\n", __LINE__, #c, errno); \
	setsockopt(fw, SOL_RANIX_FILTER, NF_RULE, "flush", 5); exit(1); } } while (0)

static int fw;

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void rule(const char *fmt, int port)
{
	char b[64];
	int n = snprintf(b, sizeof(b), fmt, port);
	CHECK(setsockopt(fw, SOL_RANIX_FILTER, NF_RULE, b, (socklen_t)n) == 0);
}

static void flush(void)
{
	CHECK(setsockopt(fw, SOL_RANIX_FILTER, NF_RULE, "flush", 5) == 0);
}

/* a connected pair: *cp the client end, return value the accepted end */
static int pair(int *cp, int port)
{
	int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	CHECK(bind(ls, (struct sockaddr *)&a, sizeof(a)) == 0);
	CHECK(listen(ls, 1) == 0);
	*cp = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(connect(*cp, (struct sockaddr *)&a, sizeof(a)) == 0);
	int s = accept(ls, NULL, NULL);
	CHECK(s >= 0);
	close(ls);
	return s;
}

static int local_port(int s)
{
	struct sockaddr_in a;
	socklen_t l = sizeof(a);
	CHECK(getsockname(s, (struct sockaddr *)&a, &l) == 0);
	return ntohs(a.sin_port);
}

/* after our shutdown(SHUT_WR) the other direction carries on, to the last byte */
static void half_close(void)
{
	int c, s = pair(&c, 4301);
	static char b[65536];
	CHECK(shutdown(c, SHUT_WR) == 0);
	CHECK(read(s, b, 1) == 0);
	long sent = 0;
	pid_t pid = fork();
	if (pid == 0) {
		while (sent < 2 * MIB) {
			ssize_t w = write(s, b, sizeof(b));
			if (w <= 0) _exit(1);
			sent += w;
		}
		_exit(0);
	}
	long got = 0;
	alarm(30);
	while (got < 2 * MIB) {
		ssize_t r = read(c, b, sizeof(b));
		CHECK(r > 0);
		got += r;
	}
	int st;
	waitpid(pid, &st, 0);
	CHECK(WIFEXITED(st) && !WEXITSTATUS(st));
	close(s);
	CHECK(read(c, b, 1) == 0);
	close(c);
}

/* A full window, then the update that reopens it lost: without a probe from the
 * sender both ends wait for each other for ever. */
static void zero_window(void)
{
	int c, s = pair(&c, 4302);
	int cport = local_port(c);
	static char b[65536];
	pid_t pid = fork();
	if (pid == 0) {
		for (long sent = 0; sent < 2 * MIB; ) {
			ssize_t w = write(c, b, sizeof(b));
			if (w <= 0) _exit(1);
			sent += w;
		}
		_exit(0);
	}
	sleep(1);                        /* the sender is blocked on a closed window */
	rule("drop tcp dport %d", cport); /* what goes to it is lost */
	long got = 0;
	double until = now() + 1;
	while (now() < until) {          /* room again, and the news of it never arrives */
		struct pollfd p = { s, POLLIN, 0 };
		if (poll(&p, 1, 50) > 0) {
			ssize_t r = read(s, b, sizeof(b));
			CHECK(r > 0);
			got += r;
		}
	}
	flush();
	alarm(40);
	while (got < 2 * MIB) {
		ssize_t r = read(s, b, sizeof(b));
		CHECK(r > 0);
		got += r;
	}
	int st;
	waitpid(pid, &st, 0);
	CHECK(WIFEXITED(st) && !WEXITSTATUS(st));
	close(s);
	close(c);
}

/* close() returns with data on its way; it still arrives, however late */
static void close_with_data(void)
{
	int c, s = pair(&c, 4303);
	int sport = local_port(s);
	static char b[65536];
	rule("drop tcp dport %d", sport);
	CHECK(write(c, b, 8192) == 8192);
	double t = now();
	close(c);
	CHECK(now() - t < 0.5);
	sleep(1);
	flush();
	long got = 0;
	alarm(30);
	for (;;) {
		ssize_t r = read(s, b, sizeof(b));
		CHECK(r >= 0);
		if (!r) break;
		got += r;
	}
	CHECK(got == 8192);
	close(s);
}

/* the path dies one way: the sender runs out of retries, reports it, and tells a
 * peer that can still hear it */
static void give_up(void)
{
	int c, s = pair(&c, 4304);
	int cport = local_port(c);
	static char b[1024];
	CHECK(write(c, b, 1) == 1 && read(s, b, 1) == 1);   /* a measured RTT: the short RTO */
	usleep(50000);
	CHECK(write(s, b, 1) == 1 && read(c, b, 1) == 1);
	rule("drop tcp dport %d", cport);
	CHECK(write(c, b, sizeof(b)) == sizeof(b));
	alarm(125);
	double t = now();
	ssize_t got = 0;
	for (;;) {
		ssize_t r = read(s, b, sizeof(b));
		if (r <= 0) { CHECK(r < 0 && errno == ECONNRESET); break; }
		got += r;
	}
	CHECK(got == sizeof(b));
	CHECK(now() - t > 20);
	flush();
	CHECK(read(c, b, 1) == -1 && errno == ETIMEDOUT);
	CHECK(read(c, b, 1) == 0);
	CHECK(write(c, b, 1) == -1 && errno == EPIPE);
	close(c);
	close(s);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGPIPE, SIG_IGN);
	fw = socket(AF_INET, SOCK_DGRAM, 0);
	flush();
	half_close();    puts("half close done");
	zero_window();   puts("zero window done");
	close_with_data(); puts("close done");
	give_up();
	puts("OK");
	return 0;
}
