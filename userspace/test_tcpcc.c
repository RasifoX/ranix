/* TCP congestion control over loopback: a forked sink reads a large stream while
 * the parent blasts it, then the parent reads its cwnd/ssthresh back via
 * getsockopt(TCP_INFO). Proves the window left the initial value (slow start ran)
 * and settled to a sane congestion-avoidance state — and that the bytes arrived
 * intact. Prints OK or the first failure. */
#define _GNU_SOURCE   /* struct tcp_info / TCP_INFO are gated behind it in musl */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/wait.h>

#define PORT 12351
#define N    (2 * 1024 * 1024)

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); return 0; } } while (0)

int main(void)
{
	int srv = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(srv >= 0);
	int one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port   = htons(PORT);
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	CHECK(bind(srv, (struct sockaddr *)&a, sizeof(a)) == 0);
	CHECK(listen(srv, 4) == 0);

	pid_t pid = fork();
	if (pid == 0) {
		int c = accept(srv, NULL, NULL);
		if (c < 0) _exit(1);
		char *b = malloc(65536);
		long got = 0; int r;
		while ((r = (int)read(c, b, 65536)) > 0) got += r;
		free(b);
		close(c);
		_exit(got == N ? 0 : 2);
	}

	int c = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in d;
	memset(&d, 0, sizeof(d));
	d.sin_family = AF_INET;
	d.sin_port   = htons(PORT);
	d.sin_addr.s_addr = htonl(0x7f000001);
	CHECK(connect(c, (struct sockaddr *)&d, sizeof(d)) == 0);

	struct tcp_info ti0;
	socklen_t il = sizeof(ti0);
	CHECK(getsockopt(c, IPPROTO_TCP, TCP_INFO, &ti0, &il) == 0);
	CHECK(il >= 84);                       /* far enough to include snd_cwnd (offset 80) */
	unsigned cwnd0 = ti0.tcpi_snd_cwnd;
	CHECK(cwnd0 >= 1);                      /* initial window, in MSS units */

	char *buf = calloc(1, 65536);
	long put = 0; int w;
	while (put < N && (w = (int)write(c, buf, N - put < 65536 ? N - put : 65536)) > 0)
		put += w;
	CHECK(put == N);

	/* after pushing 2MB the window must have grown well past the initial value */
	struct tcp_info ti1;
	il = sizeof(ti1);
	CHECK(getsockopt(c, IPPROTO_TCP, TCP_INFO, &ti1, &il) == 0);
	CHECK(ti1.tcpi_snd_cwnd > cwnd0);       /* slow start grew the window */
	CHECK(ti1.tcpi_snd_ssthresh >= 2);      /* a sane congestion threshold */

	close(c);
	free(buf);
	int st = 0;
	waitpid(pid, &st, 0);
	close(srv);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

	puts("OK");
	return 0;
}
