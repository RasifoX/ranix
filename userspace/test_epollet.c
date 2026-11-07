/* Edge-triggered epoll: data arriving after a reader drained the socket is an edge,
 * even if the fd looked ready the last time. The X server sleeps on this. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define ROUNDS 3000

static int sv[2];
static void *echo(void *a)
{
	char c;
	for (int i = 0; i < ROUNDS; i++) {
		while (read(sv[1], &c, 1) != 1) ;
		write(sv[1], "x", 1);
		write(sv[1], "y", 1);
	}
	return a;
}

int main(void)
{
	socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv);
	int fl = 0; (void)fl;
	pthread_t t;
	pthread_create(&t, NULL, echo, NULL);
	int ep = epoll_create1(0);
	struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.fd = sv[0]};
	epoll_ctl(ep, EPOLL_CTL_ADD, sv[0], &ev);
	for (int i = 0; i < ROUNDS; i++) {
		write(sv[0], "p", 1);
		int got = 0;
		char b[8];
		while (got < 2) {
			ssize_t n;
			while ((n = read(sv[0], b, sizeof b)) > 0) got += (int)n;
			if (got < 2 && epoll_wait(ep, &ev, 1, 3000) != 1) { printf("FAIL round %d got %d\n", i, got); return 1; }
		}
	}
	puts("OK");
	return 0;
}
