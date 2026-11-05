/* shutdown(2) on an AF_UNIX stream: half-close both ways. It used to run the TCP
 * close on a socket with no device, which took the kernel down. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define CHECK(c, w) do { if (!(c)) { printf("FAIL %s (errno %d)\n", w, errno); fails++; } } while (0)

int main(void)
{
	int sv[2];
	char b[8];
	signal(SIGPIPE, SIG_IGN);

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
	CHECK(write(sv[0], "abc", 3) == 3, "write");
	CHECK(shutdown(sv[0], SHUT_WR) == 0, "SHUT_WR");
	CHECK(write(sv[0], "x", 1) == -1 && errno == EPIPE, "write after SHUT_WR");
	CHECK(read(sv[1], b, sizeof(b)) == 3, "queued data survives");
	CHECK(read(sv[1], b, sizeof(b)) == 0, "then EOF");
	CHECK(write(sv[1], "de", 2) == 2, "the other way stays open");
	CHECK(read(sv[0], b, sizeof(b)) == 2, "and reads");
	close(sv[0]); close(sv[1]);

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
	struct pollfd p = { sv[1], POLLIN, 0 };
	pid_t c = fork();
	if (c == 0) {   /* a blocked reader wakes */
		char r;
		_exit(read(sv[1], &r, 1) == 0 ? 0 : 1);
	}
	usleep(50000);
	CHECK(shutdown(sv[0], SHUT_RDWR) == 0, "SHUT_RDWR");
	int st = 0;
	waitpid(c, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "blocked reader gets EOF");
	CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLIN), "poll sees EOF");
	CHECK(read(sv[0], b, 1) == 0, "SHUT_RD reads EOF");
	close(sv[0]); close(sv[1]);

	/* with no peer left, and on an unconnected socket */
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
	close(sv[1]);
	CHECK(shutdown(sv[0], SHUT_RDWR) == 0, "shutdown with the peer gone");
	close(sv[0]);
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	shutdown(s, SHUT_RDWR);
	close(s);

	if (fails) { printf("FAILED %d\n", fails); return 1; }
	puts("OK");
	return 0;
}
