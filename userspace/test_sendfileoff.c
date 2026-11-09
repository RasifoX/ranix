/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* sendfile with an offset reads from there, writes the new offset back and
 * leaves the input file's own position alone; a negative offset is EINVAL;
 * an out_fd that is not open for writing, or not open, is EBADF (LTP
 * sendfile02, sendfile03, sendfile05). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void)
{
	int bad = 0;
	int in = open("/tmp/sfo-in", O_CREAT | O_RDWR | O_TRUNC, 0644);
	write(in, "abcdefghij", 10);
	lseek(in, 2, SEEK_SET);
	int out = open("/tmp/sfo-out", O_CREAT | O_RDWR | O_TRUNC, 0644);
	off_t off = 5;
	long n = sendfile(out, in, &off, 3);
	char got[8] = { 0 };
	pread(out, got, 3, 0);
	if (n != 3 || strcmp(got, "fgh")) { printf("sendfile from 5: %ld '%s'\n", n, got); bad++; }
	if (off != 8) { printf("offset written back: %ld, want 8\n", (long)off); bad++; }
	if (lseek(in, 0, SEEK_CUR) != 2) { printf("in_fd position moved to %ld\n", (long)lseek(in, 0, SEEK_CUR)); bad++; }
	off = -1;
	errno = 0;
	if (sendfile(out, in, &off, 1) != -1 || errno != EINVAL) { puts("negative offset: not EINVAL"); bad++; }
	int ro = open("/tmp/sfo-out", O_RDONLY);
	errno = 0;
	if (sendfile(ro, in, NULL, 1) != -1 || errno != EBADF) { puts("read-only out_fd: not EBADF"); bad++; }
	errno = 0;
	if (sendfile(-1, in, NULL, 1) != -1 || errno != EBADF) { puts("out_fd -1: not EBADF"); bad++; }
	/* a socket has no access mode: it is written, not refused */
	int sv[2];
	socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
	off = 0;
	if (sendfile(sv[0], in, &off, 4) != 4) { printf("sendfile to a socket: errno %d\n", errno); bad++; }
	unlink("/tmp/sfo-in"); unlink("/tmp/sfo-out");
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
