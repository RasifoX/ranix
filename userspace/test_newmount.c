/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The new mount API (open_tree 428 .. fspick 433) is not provided: each call gets
 * ENOSYS, as mount(8) expects before it falls back to mount(2), and the kernel log
 * says so once instead of a warning per call. First failure or OK. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static char log[1 << 19];

static int count(const char *needle)
{
	int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (fd < 0) { puts("FAIL kmsg"); exit(1); }
	size_t have = 0;
	ssize_t r;
	while (have < sizeof(log) - 1 && (r = read(fd, log + have, sizeof(log) - 1 - have)) > 0)
		have += (size_t)r;
	close(fd);
	log[have] = 0;
	int n = 0;
	for (const char *p = log; (p = strstr(p, needle)); p += strlen(needle)) n++;
	return n;
}

int main(void)
{
	int base = count("syscall: unimplemented 43") + count("syscall: unimplemented 428") +
	           count("syscall: unimplemented 429");
	for (int round = 0; round < 3; round++)
		for (long n = 428; n <= 433; n++) {
			errno = 0;
			long r = syscall(n, -1L, 0L, 0L, 0L, 0L);
			if (r != -1 || errno != ENOSYS) {
				printf("FAIL %ld gave %ld errno %d\n", n, r, errno);
				return 1;
			}
		}
	int warned = count("syscall: unimplemented 43") + count("syscall: unimplemented 428") +
	             count("syscall: unimplemented 429") - base;
	if (warned) { printf("FAIL %d unimplemented lines for 428-433\n", warned); return 1; }
	int once = count("new mount API (428-433) not provided");
	if (once != 1) { printf("FAIL the note came %d times\n", once); return 1; }
	puts("OK");
	return 0;
}
