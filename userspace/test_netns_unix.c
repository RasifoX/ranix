/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* AF_UNIX names are per network namespace: abstract and path binds neither reach
 * across nor collide, a namespace filling its table leaves the others alone, and a
 * socket keeps its namespace alive after the last process in it is gone. */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#define ABS "@ranix-nsu"   /* '@' becomes the leading NUL */
#define PATHNAME "/tmp/ranix-nsu.sock"

static socklen_t fill(struct sockaddr_un *a, const char *name)
{
	memset(a, 0, sizeof(*a));
	a->sun_family = AF_UNIX;
	strcpy(a->sun_path, name);
	socklen_t l = offsetof(struct sockaddr_un, sun_path) + strlen(name) + 1;
	if (name[0] == '@') { a->sun_path[0] = 0; l--; }
	return l;
}

static int listener(const char *name)
{
	struct sockaddr_un a;
	socklen_t l = fill(&a, name);
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (bind(s, (void *)&a, l) || listen(s, 4)) { close(s); return -1; }
	return s;
}

static int dial(const char *name)
{
	struct sockaddr_un a;
	socklen_t l = fill(&a, name);
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	int r = connect(s, (void *)&a, l) ? errno : 0;
	close(s);
	return r;
}

static int inside(void)
{
	if (unshare(CLONE_NEWNET)) return 1;
	if (dial(ABS) != ECONNREFUSED) return 2;
	if (dial(PATHNAME) != ECONNREFUSED) return 3;
	int a = listener(ABS), b = listener(PATHNAME);
	if (a < 0 || b < 0) return 4;
	if (dial(ABS) || dial(PATHNAME)) return 5;
	return 0;
}

/* holds every slot this namespace has, and shows the host is unharmed */
static int quota(int to_host)
{
	if (unshare(CLONE_NEWNET)) return 1;
	int n = 0, err = 0;
	char name[32];
	for (;; n++) {
		struct sockaddr_un a;
		snprintf(name, sizeof(name), "@q%d", n);
		socklen_t l = fill(&a, name);
		int s = socket(AF_UNIX, SOCK_STREAM, 0);
		if (s < 0) { err = errno; break; }
		if (bind(s, (void *)&a, l)) { err = errno; close(s); break; }
		if (n > 4096) return 2;
	}
	if (err != ENOSPC) { printf("quota stopped at %d errno=%d\n", n, err); return 3; }
	printf("namespace quota %d\n", n);
	char b;
	write(to_host, "x", 1);
	read(to_host, &b, 1);
	return 0;
}

/* a socket from a namespace whose last process has gone */
static int send_fd(int via)
{
	if (unshare(CLONE_NEWNET)) return 1;
	int s = listener("@orphan");
	if (s < 0) return 2;
	char cb[CMSG_SPACE(sizeof(int))], c = 'f';
	struct iovec io = { &c, 1 };
	struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof(cb) };
	struct cmsghdr *h = CMSG_FIRSTHDR(&m);
	h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(h), &s, sizeof(int));
	return sendmsg(via, &m, 0) == 1 ? 0 : 3;
}

static int wait_rc(pid_t c)
{
	int st;
	waitpid(c, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}

#define FAIL(what) do { printf("FAIL %s\n", what); return 1; } while (0)

int main(void)
{
	mkdir("/tmp", 0755);
	int a = listener(ABS), b = listener(PATHNAME);
	if (a < 0 || b < 0) FAIL("listen");
	if (dial(ABS) || dial(PATHNAME)) FAIL("host dial");

	pid_t c = fork();
	if (c == 0) _exit(inside());
	int r = wait_rc(c);
	if (r) { printf("FAIL inside rc=%d\n", r); return 1; }
	if (dial(ABS) || dial(PATHNAME)) FAIL("host dial after");
	printf("scope ok\n");

	int sv[2];
	socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
	c = fork();
	if (c == 0) _exit(quota(sv[1]));
	char x;
	if (read(sv[0], &x, 1) != 1) { printf("FAIL quota rc=%d\n", wait_rc(c)); return 1; }
	if (listener("@host-after-quota") < 0) FAIL("host blocked by a namespace's binds");
	write(sv[0], "x", 1);
	r = wait_rc(c);
	if (r) { printf("FAIL quota rc=%d\n", r); return 1; }
	printf("quota ok\n");

	c = fork();
	if (c == 0) _exit(send_fd(sv[1]));
	char cb[CMSG_SPACE(sizeof(int))], d;
	struct iovec io = { &d, 1 };
	struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof(cb) };
	if (recvmsg(sv[0], &m, 0) != 1) FAIL("recv fd");
	r = wait_rc(c);
	if (r) { printf("FAIL send rc=%d\n", r); return 1; }
	int orphan;
	memcpy(&orphan, CMSG_DATA(CMSG_FIRSTHDR(&m)), sizeof(int));
	usleep(100000);
	/* churn the heap over whatever the namespace held, then touch the socket */
	for (int i = 0; i < 8; i++) { c = fork(); if (c == 0) _exit(unshare(CLONE_NEWNET)); wait_rc(c); }
	struct sockaddr_un got;
	socklen_t gl = sizeof(got);
	if (getsockname(orphan, (void *)&got, &gl)) FAIL("getsockname");
	close(orphan);   /* removes the binding from the namespace's table */
	printf("pin ok\nOK\n");
	return 0;
}
