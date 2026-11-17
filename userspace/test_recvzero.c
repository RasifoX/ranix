/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A zero-length receive on a stream socket that has data waiting returns 0 at
 * once and leaves the data where it was, as Linux does. Ranix looped in the
 * kernel for good (found by test_fuzz: a "soft lockup" in do_recviov): the
 * copy loop took nothing for a length of 0, and the "data came in meanwhile"
 * check sent it round again. AF_UNIX and TCP over loopback; recv, recvfrom
 * and recvmsg with an empty iovec. Each call runs in a child the parent ends
 * after 3 s, so a hang is reported, not waited for. Prints OK or FAIL lines. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>

static int fails;
#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } while (0)

/* 0 the call returned 0 and the byte is still there, 1 wrong, 2 hung */
static int zero_recv(int rd, int how)
{
	pid_t p = fork();
	if (p == 0) {
		char b[4];
		ssize_t r;
		if (how == 0) r = recv(rd, b, 0, 0);
		else if (how == 1) r = recvfrom(rd, b, 0, 0, NULL, NULL);
		else {
			struct msghdr m;
			memset(&m, 0, sizeof(m));
			r = recvmsg(rd, &m, 0);
		}
		if (r != 0) _exit(1);
		if (recv(rd, b, 1, MSG_DONTWAIT) != 1 || b[0] != 'x') _exit(1);
		_exit(0);
	}
	for (int i = 0; i < 300; i++) {
		int st;
		if (waitpid(p, &st, WNOHANG) == p) return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
		usleep(10000);
	}
	kill(p, SIGKILL);
	waitpid(p, NULL, 0);
	return 2;
}

static int tcp_pair(int sv[2])
{
	int l = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t al = sizeof(a);
	if (l < 0 || bind(l, (struct sockaddr *)&a, sizeof(a)) || listen(l, 1) ||
	    getsockname(l, (struct sockaddr *)&a, &al)) return -1;
	sv[0] = socket(AF_INET, SOCK_STREAM, 0);
	if (sv[0] < 0 || connect(sv[0], (struct sockaddr *)&a, sizeof(a))) return -1;
	sv[1] = accept(l, NULL, NULL);
	close(l);
	return sv[1] < 0 ? -1 : 0;
}

int main(void)
{
	static const char *const kind[] = { "AF_UNIX", "TCP" };
	static const char *const call[] = { "recv", "recvfrom", "recvmsg" };
	for (int k = 0; k < 2; k++)
		for (int how = 0; how < 3; how++) {
			int sv[2];
			if (k == 0 ? socketpair(AF_UNIX, SOCK_STREAM, 0, sv) : tcp_pair(sv)) {
				FAIL("%s pair: %s", kind[k], strerror(errno));
				continue;
			}
			if (write(sv[0], "x", 1) != 1) FAIL("%s write", kind[k]);
			usleep(20000);   /* the byte in the receive queue */
			int r = zero_recv(sv[1], how);
			if (r == 2) FAIL("%s %s of 0 bytes with data waiting hangs", kind[k], call[how]);
			else if (r) FAIL("%s %s of 0 bytes: not 0, or the data was taken", kind[k], call[how]);
			close(sv[0]);
			close(sv[1]);
		}
	printf(fails ? "FAILED %d\n" : "OK\n", fails);
	return fails != 0;
}
