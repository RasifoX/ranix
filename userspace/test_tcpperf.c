/* TCP throughput probe over loopback: a forked sender blasts N bytes, the parent
 * receives them and times it. Reports MB/s — the stack's processing ceiling. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>

#define PORT 12347
#define N    (8 * 1024 * 1024)

int main(void)
{
	int srv = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(srv, (struct sockaddr *)&a, sizeof(a)) < 0) { puts("FAIL bind"); return 1; }
	listen(srv, 4);

	pid_t pid = fork();
	if (pid == 0) {
		int c = accept(srv, NULL, NULL);
		char *b = calloc(1, 65536);
		int left = N, w;
		while (left > 0 && (w = (int)write(c, b, left < 65536 ? left : 65536)) > 0) left -= w;
		close(c);
		_exit(0);
	}

	int c = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in d;
	memset(&d, 0, sizeof(d));
	d.sin_family = AF_INET; d.sin_port = htons(PORT); d.sin_addr.s_addr = htonl(0x7f000001);
	if (connect(c, (struct sockaddr *)&d, sizeof(d)) < 0) { puts("FAIL connect"); return 1; }

	char *buf = malloc(65536);
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int got = 0, r;
	while (got < N && (r = (int)read(c, buf, 65536)) > 0) got += r;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	close(c);
	waitpid(pid, 0, 0);
	close(srv);

	double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	if (secs <= 0) secs = 1e-6;
	printf("PERF %d bytes in %.3f s = %.2f MB/s\n", got, secs, (got / 1048576.0) / secs);
	return 0;
}
