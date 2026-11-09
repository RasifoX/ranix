/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /proc/sys/kernel/ranix_deadline: the boot deadline as seconds left, 0 = off.
 * A healthy boot writes 0 to turn it off; writing N re-arms it N seconds ahead.
 * Checks: the test VM boots with no deadline (reads 0); arming reads back a value
 * just under N; 0 disarms; garbage, an empty write and an absurd value are
 * EINVAL; an unprivileged writer gets EPERM and the value does not change. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define P "/proc/sys/kernel/ranix_deadline"

static long rd(void)
{
	char b[32] = {0};
	int fd = open(P, O_RDONLY);
	if (fd < 0) return -1;
	int n = (int)read(fd, b, sizeof b - 1);
	close(fd);
	if (n <= 0) return -2;
	return strtol(b, NULL, 10);
}

static int wr(const char *s)
{
	int fd = open(P, O_WRONLY);
	if (fd < 0) return -errno;
	int r = (int)write(fd, s, strlen(s));
	int e = errno;
	close(fd);
	return r < 0 ? -e : 0;
}

#define FAIL(...) do { printf("FAIL " __VA_ARGS__); printf("\n"); return 1; } while (0)

int main(void)
{
	long v = rd();
	if (v != 0) FAIL("initial %ld", v);
	if (wr("3600\n")) FAIL("arm");
	v = rd();
	if (v < 3590 || v > 3600) FAIL("armed reads %ld", v);
	if (wr("abc") != -EINVAL) FAIL("garbage not EINVAL");
	if (wr("") != -EINVAL) FAIL("empty not EINVAL");
	if (wr("99999999") != -EINVAL) FAIL("huge not EINVAL");
	pid_t p = fork();
	if (p == 0) {
		if (setgid(65534) || setuid(65534)) _exit(3);
		int r = wr("5");
		_exit(r == -EPERM || r == -EACCES ? 0 : 4);
	}
	int st = 0;
	waitpid(p, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) FAIL("unprivileged write st=%d", st);
	v = rd();
	if (v < 3500) FAIL("unprivileged write changed it to %ld", v);
	if (wr("0")) FAIL("disarm");
	if ((v = rd()) != 0) FAIL("after disarm %ld", v);
	printf("OK\n");
	return 0;
}
