/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* epoll's refusals, as on Linux (LTP epoll_create02, epoll_create1_02,
 * epoll_ctl02/04/05/07, epoll_wait03): a size <= 0 or an unknown flag is
 * EINVAL; an fd that is not an epoll is EINVAL where an epoll is wanted; a
 * regular file or directory cannot be watched (EPERM); an epoll that would
 * contain itself is ELOOP, and epolls nest at most 5 deep. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <unistd.h>

static int bad;

static void want(const char *what, long r, int err)
{
	int got = r < 0 ? errno : 0;
	if (got != err) {
		printf("%s: want %s, got %s\n", what, err ? strerror(err) : "success",
		       got ? strerror(got) : "success");
		bad++;
	}
}

static int add(int ep, int fd)
{
	struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
	return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

int main(void)
{
	struct epoll_event ev;
	int p[2], chain[6];
	pipe(p);
	int file = open("/tmp/epollerr", O_CREAT | O_RDWR, 0600);
	int dir = open("/tmp", O_RDONLY | O_DIRECTORY);

	errno = 0; want("epoll_create(0)", syscall(SYS_epoll_create, 0), EINVAL);
	errno = 0; want("epoll_create(-1)", syscall(SYS_epoll_create, -1), EINVAL);
	errno = 0; want("epoll_create1(-1)", syscall(SYS_epoll_create1, -1), EINVAL);
	errno = 0; want("epoll_create1(CLOEXEC+1)", syscall(SYS_epoll_create1, EPOLL_CLOEXEC + 1), EINVAL);

	int ep = epoll_create1(0);
	errno = 0; want("wait on a pipe fd", epoll_wait(p[0], &ev, 1, 0), EINVAL);
	errno = 0; want("wait on a closed fd", epoll_wait(1000, &ev, 1, 0), EBADF);
	errno = 0; want("ctl on a pipe fd", add(p[0], p[1]), EINVAL);
	errno = 0; want("watch a file", add(ep, file), EPERM);
	errno = 0; want("watch a directory", add(ep, dir), EPERM);
	errno = 0; want("watch a pipe", add(ep, p[0]), 0);
	/* an anonymous inode may look like a file, but it is not one (LTP epoll_pwait06) */
	char params[120] = { 0 };
	int ring = (int)syscall(425 /* io_uring_setup */, 4, params);
	if (ring >= 0) { errno = 0; want("watch an io_uring", add(ep, ring), 0); }

	int a = epoll_create1(0), b = epoll_create1(0);
	errno = 0; want("a in b", add(b, a), 0);
	errno = 0; want("b in a (cycle)", add(a, b), ELOOP);

	/* chain[0] holds the pipe, chain[i] holds chain[i-1]: 5 epolls deep */
	for (int i = 0; i < 5; i++) {
		chain[i] = epoll_create1(0);
		errno = 0; want("build chain", add(chain[i], i ? chain[i - 1] : p[0]), 0);
	}
	chain[5] = epoll_create1(0);
	errno = 0;
	long r = add(chain[5], chain[4]);
	if (!(r < 0 && (errno == EINVAL || errno == ELOOP))) {
		printf("6 deep: want EINVAL or ELOOP, got %s\n", r < 0 ? strerror(errno) : "success");
		bad++;
	}
	errno = 0; want("top into bottom (cycle)", add(chain[0], chain[4]), ELOOP);
	unlink("/tmp/epollerr");
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
