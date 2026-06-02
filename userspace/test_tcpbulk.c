/* Bulk TCP over loopback: a forked server reads N bytes and echoes them back;
 * the parent sends a known pattern, reads it all back, and checks integrity.
 * Exercises multi-segment send/receive (window, retransmit, reassembly) beyond
 * the single-segment handshake test. First failure or OK. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>

#define PORT 12346
#define N    (64 * 1024)

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); return 0; } } while (0)

static unsigned char pat(int i) { return (unsigned char)((i * 1103515245u + 12345u) >> 16); }

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
		/* server: read N, echo back */
		int c = accept(srv, NULL, NULL);
		if (c < 0) _exit(1);
		char *b = malloc(N);
		int got = 0, r;
		while (got < N && (r = (int)read(c, b + got, N - got)) > 0) got += r;
		int put = 0, w;
		while (put < got && (w = (int)write(c, b + put, got - put)) > 0) put += w;
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

	char *out = malloc(N), *in = malloc(N);
	for (int i = 0; i < N; i++) out[i] = pat(i);

	/* send and receive concurrently would need a thread; instead send all then
	 * read all — loopback buffers are large enough for this size. */
	int put = 0, w;
	while (put < N && (w = (int)write(c, out + put, N - put)) > 0) put += w;
	CHECK(put == N);

	int got = 0, r;
	while (got < N && (r = (int)read(c, in + got, N - got)) > 0) got += r;
	CHECK(got == N);
	CHECK(memcmp(out, in, N) == 0);

	close(c);
	int st = 0;
	waitpid(pid, &st, 0);
	close(srv);
	free(out); free(in);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

	puts("OK");
	return 0;
}
