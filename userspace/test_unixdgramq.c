/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* An AF_UNIX datagram socket's receive queue is bounded, as on Linux
 * (net.unix.max_dgram_qlen, 512): a nonblocking sender gets EAGAIN instead of
 * filling memory, and sendfile into it is EAGAIN too (LTP sendfile07). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void)
{
	int p[2], bad = 0, i;
	if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, p)) { puts("socketpair failed"); return 1; }
	for (i = 0; i < 100000; i++) {
		if (write(p[1], "a", 1) < 0) break;
	}
	if (i == 100000 || errno != EAGAIN) { printf("after %d datagrams: errno %d, want EAGAIN\n", i, errno); bad++; }
	int f = open("/tmp/unixdgramq", O_CREAT | O_RDWR | O_TRUNC, 0644);
	write(f, "0123456789", 10);
	lseek(f, 0, SEEK_SET);
	errno = 0;
	if (sendfile(p[1], f, NULL, 1) != -1 || errno != EAGAIN) { printf("sendfile into the full socket: errno %d, want EAGAIN\n", errno); bad++; }
	char c;
	read(p[0], &c, 1);   /* room for one again */
	if (write(p[1], "b", 1) != 1) { printf("after a read: write errno %d\n", errno); bad++; }
	close(f); unlink("/tmp/unixdgramq");
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
