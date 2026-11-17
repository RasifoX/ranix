/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A write to /dev/kmsg is one log record, at most 1024 bytes; a longer one is
 * EINVAL (Linux, PRINTKRB_RECORD_MAX). Ranix took any length and printed it
 * a character at a time to the console without giving the CPU up: a fuzzer's
 * large write held a CPU in the kernel for 35 s (softlockup in kmsg_write).
 * Checks: 1024 bytes go in, 1025 and 1 MB are EINVAL at once.
 * Runs in a child with a time limit. Prints OK or FAIL lines. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int check(void)
{
	int fd = open("/dev/kmsg", O_WRONLY);
	if (fd < 0) { printf("FAIL: open /dev/kmsg: %s\n", strerror(errno)); return 1; }
	static char b[1 << 20];
	memset(b, 'k', sizeof(b));
	memcpy(b, "test_kmsglen ", 13);
	int bad = 0;
	b[1023] = '\n';
	ssize_t r = write(fd, b, 1024);
	if (r != 1024) { printf("FAIL: 1024-byte record: %zd errno %d\n", r, errno); bad = 1; }
	b[1023] = 'k';
	errno = 0;
	r = write(fd, b, 1025);
	if (r != -1 || errno != EINVAL) { printf("FAIL: 1025-byte record: %zd errno %d, want EINVAL\n", r, errno); bad = 1; }
	errno = 0;
	r = write(fd, b, sizeof(b));
	if (r != -1 || errno != EINVAL) { printf("FAIL: 1 MB record: %zd errno %d, want EINVAL\n", r, errno); bad = 1; }
	close(fd);
	return bad;
}

int main(void)
{
	fflush(stdout);
	pid_t c = fork();
	if (c == 0) { alarm(10); _exit(check()); }
	int st = 0;
	waitpid(c, &st, 0);
	int fails = 0;
	if (WIFSIGNALED(st)) { printf("FAIL: killed by signal %d (a long write held it?)\n", WTERMSIG(st)); fails = 1; }
	else if (WEXITSTATUS(st)) fails = 1;
	printf(fails ? "FAILED\n" : "OK\n");
	return fails;
}
