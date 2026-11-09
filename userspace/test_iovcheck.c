/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Vectored positioned I/O checks the whole request first: an iovec whose
 * length is past SSIZE_MAX is EINVAL before anything is read or written, and
 * preadv2/pwritev2 refuse flags they do not know with EOPNOTSUPP (LTP
 * preadv02, preadv202, pwritev02, pwritev202). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

static int bad;
static void want(const char *what, long r, int err)
{
	int got = r < 0 ? errno : 0;
	if (got != err) { printf("%s: errno %d, want %d\n", what, got, err); bad++; }
}

int main(void)
{
	char b[4];
	int fd = open("/tmp/iovck", O_CREAT | O_RDWR | O_TRUNC, 0644);
	write(fd, "abcd", 4);
	struct iovec huge = { b, (size_t)-1 }, ok = { b, 4 };
	errno = 0; want("preadv huge iov_len", preadv(fd, &huge, 1, 0), EINVAL);
	errno = 0; want("pwritev huge iov_len", pwritev(fd, &huge, 1, 0), EINVAL);
	errno = 0; want("preadv2 flags -1", syscall(SYS_preadv2, fd, &ok, 1, 0L, 0L, -1), EOPNOTSUPP);
	errno = 0; want("pwritev2 flags -1", syscall(SYS_pwritev2, fd, &ok, 1, 0L, 0L, -1), EOPNOTSUPP);
	errno = 0; want("preadv2 good", syscall(SYS_preadv2, fd, &ok, 1, 0L, 0L, 0), 0);
	unlink("/tmp/iovck");
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
