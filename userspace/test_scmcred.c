/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* SCM_CREDENTIALS: a socket that set SO_PASSCRED hears who sent each message,
 * next to any passed descriptors. Chromium's zygote learns its children's real
 * pids so (RecvMsgWithPid) and VS Code could not open a window without it. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>

static int recv_one(int s, pid_t *pid, int *fd)
{
	char buf[16], ctl[CMSG_SPACE(sizeof(struct ucred)) + CMSG_SPACE(sizeof(int))];
	struct iovec v = {buf, sizeof buf};
	struct msghdr m = {0};
	m.msg_iov = &v; m.msg_iovlen = 1; m.msg_control = ctl; m.msg_controllen = sizeof ctl;
	*pid = -1; *fd = -1;
	if (recvmsg(s, &m, 0) <= 0) return -1;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS) {
			struct ucred u; memcpy(&u, CMSG_DATA(c), sizeof u); *pid = u.pid;
		} else if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
			memcpy(fd, CMSG_DATA(c), sizeof(int));
		}
	}
	return 0;
}

int main(void)
{
	int sv[2], one = 1;
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv)) { printf("FAIL socketpair\n"); return 0; }
	if (setsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &one, sizeof one)) { printf("FAIL passcred\n"); return 0; }
	pid_t c = fork();
	if (c == 0) {
		if (write(sv[1], "plain", 5) != 5) _exit(1);
		int p[2];
		if (pipe(p)) _exit(1);
		char ctl[CMSG_SPACE(sizeof(int))];
		struct iovec v = {"fd", 2};
		struct msghdr m = {0};
		m.msg_iov = &v; m.msg_iovlen = 1; m.msg_control = ctl; m.msg_controllen = sizeof ctl;
		struct cmsghdr *h = CMSG_FIRSTHDR(&m);
		h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(h), &p[1], sizeof(int));
		_exit(sendmsg(sv[1], &m, 0) == 2 ? 0 : 1);
	}
	pid_t a, b; int fa, fb;
	if (recv_one(sv[0], &a, &fa) || recv_one(sv[0], &b, &fb)) { printf("FAIL recv\n"); return 0; }
	int st;
	waitpid(c, &st, 0);
	if (a != c || b != c) { printf("FAIL pid %d %d want %d\n", (int)a, (int)b, (int)c); return 0; }
	if (fa != -1 || fb < 0 || fcntl(fb, F_GETFD) < 0) { printf("FAIL fds %d %d\n", fa, fb); return 0; }
	printf("OK\n");
	return 0;
}
