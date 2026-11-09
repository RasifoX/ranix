/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* More refusals Linux makes (LTP pipe03, waitpid04, mmap08, mmap20): a pipe
 * end used the wrong way is EBADF; waitpid with unknown options is EINVAL and
 * with pid INT_MIN ESRCH; mmap of a closed fd is EBADF; MAP_SHARED_VALIDATE
 * with a flag it does not know is EOPNOTSUPP. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MAP_SHARED_VALIDATE
#define MAP_SHARED_VALIDATE 0x03
#endif

static int bad;
static void want(const char *what, long r, int err)
{
	int got = r < 0 ? errno : 0;
	if (got != err) { printf("%s: errno %d, want %d\n", what, got, err); bad++; }
}

int main(void)
{
	int p[2];
	char c = 'x';
	pipe(p);
	errno = 0; want("write to the read end", write(p[0], &c, 1), EBADF);
	errno = 0; want("read from the write end", read(p[1], &c, 1), EBADF);
	errno = 0; want("waitpid options -1", waitpid(-1, NULL, -1), EINVAL);
	errno = 0; want("waitpid INT_MIN", waitpid(INT_MIN, NULL, 0), ESRCH);
	int fd = open("/tmp/argcheck", O_CREAT | O_RDWR | O_TRUNC, 0644);
	ftruncate(fd, 4096);
	void *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED_VALIDATE | (1 << 10), fd, 0);
	errno = m == MAP_FAILED ? errno : 0;
	want("MAP_SHARED_VALIDATE unknown flag", m == MAP_FAILED ? -1 : 0, EOPNOTSUPP);
	close(fd);
	m = mmap(NULL, 4096, PROT_WRITE, MAP_SHARED, fd, 0);
	errno = m == MAP_FAILED ? errno : 0;
	want("mmap of a closed fd", m == MAP_FAILED ? -1 : 0, EBADF);
	unlink("/tmp/argcheck");
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
