/* AF_UNIX SOCK_SEQPACKET keeps each message whole and apart: a 5000-byte
 * message with a descriptor arrives in one piece (Chromium's zygote fork
 * request is ~2 KB and was cut at 1536, so no renderer ever started), a
 * message gathered from two pieces is one message, two messages never merge,
 * and a short read drops the rest of its message (MSG_TRUNC). recv() reads
 * end of file once the peer is gone: Rust's Command::spawn waits so for exec
 * to close the child's end (VS Code's proxy resolver hung there). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>

static char big[5000], in[8192];

int main(void)
{
	int sv[2], p[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) || pipe(p)) { printf("FAIL setup\n"); return 0; }
	for (int i = 0; i < (int)sizeof big; i++) big[i] = (char)(i * 7);
	char ctl[CMSG_SPACE(sizeof(int))];
	struct iovec v = {big, sizeof big};
	struct msghdr m = {0};
	m.msg_iov = &v; m.msg_iovlen = 1; m.msg_control = ctl; m.msg_controllen = sizeof ctl;
	struct cmsghdr *h = CMSG_FIRSTHDR(&m);
	h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(h), &p[1], sizeof(int));
	if (sendmsg(sv[1], &m, 0) != (ssize_t)sizeof big) { printf("FAIL big send\n"); return 0; }
	struct iovec two[2] = {{"abc", 3}, {"def", 3}};
	struct msghdr m2 = {0};
	m2.msg_iov = two; m2.msg_iovlen = 2;
	if (sendmsg(sv[1], &m2, 0) != 6) { printf("FAIL two-piece send\n"); return 0; }
	if (write(sv[1], "0123456789", 10) != 10 || write(sv[1], "next", 4) != 4) { printf("FAIL writes\n"); return 0; }

	struct iovec r = {in, sizeof in};
	char rc[CMSG_SPACE(sizeof(int))];
	struct msghdr mr = {0};
	mr.msg_iov = &r; mr.msg_iovlen = 1; mr.msg_control = rc; mr.msg_controllen = sizeof rc;
	ssize_t n = recvmsg(sv[0], &mr, 0);
	if (n != (ssize_t)sizeof big || memcmp(in, big, sizeof big)) { printf("FAIL big recv %zd\n", n); return 0; }
	struct cmsghdr *c = CMSG_FIRSTHDR(&mr);
	if (!c || c->cmsg_type != SCM_RIGHTS) { printf("FAIL big fd\n"); return 0; }
	n = read(sv[0], in, sizeof in);
	if (n != 6 || memcmp(in, "abcdef", 6)) { printf("FAIL two-piece recv %zd\n", n); return 0; }
	struct msghdr mt = {0};
	struct iovec t = {in, 3};
	mt.msg_iov = &t; mt.msg_iovlen = 1;
	n = recvmsg(sv[0], &mt, 0);
	if (n != 3 || !(mt.msg_flags & MSG_TRUNC)) { printf("FAIL trunc %zd flags %x\n", n, mt.msg_flags); return 0; }
	n = read(sv[0], in, sizeof in);
	if (n != 4 || memcmp(in, "next", 4)) { printf("FAIL after trunc %zd\n", n); return 0; }
	int pp[2];
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pp)) { printf("FAIL pair2\n"); return 0; }
	int ty = 0; socklen_t tl = sizeof ty;
	if (getsockopt(pp[0], SOL_SOCKET, SO_TYPE, &ty, &tl) || ty != SOCK_SEQPACKET) { printf("FAIL so_type %d\n", ty); return 0; }
	pid_t ch = fork();
	if (ch == 0) { close(pp[0]); usleep(100000); _exit(0); }
	close(pp[1]);
	alarm(5);
	n = recvfrom(pp[0], in, 8, 0, NULL, NULL);
	alarm(0);
	waitpid(ch, NULL, 0);
	if (n != 0) { printf("FAIL eof after peer exit %zd\n", n); return 0; }
	printf("OK\n");
	return 0;
}
