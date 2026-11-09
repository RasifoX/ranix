/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /proc/self/fd/N names where a file really is, whatever path opened it.
 *
 * open() used to keep the path as written, so a directory reached through
 * /proc/self/fd/3/tasks or through a symlink read back with the link still in
 * it. Claude Code pins its task output directory that way (O_PATH, then
 * mkdir/open through /proc/self/fd/N) and compares the readlink with the real
 * path; the mismatch made its Bash tool refuse with "task output swap refused
 * (tasks dir moved or linked)". */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

static void want(const char *what, int fd, const char *path)
{
	char p[64], b[512];
	snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
	ssize_t n = fd < 0 ? -1 : readlink(p, b, sizeof b - 1);
	if (n < 0) { printf("FAIL %s: fd=%d readlink errno=%d\n", what, fd, errno); fails++; return; }
	b[n] = 0;
	if (strcmp(b, path) != 0) { printf("FAIL %s: '%s' != '%s'\n", what, b, path); fails++; return; }
	struct stat a, l;
	if (fstat(fd, &a) != 0 || lstat(path, &l) != 0 || a.st_dev != l.st_dev || a.st_ino != l.st_ino) {
		printf("FAIL %s: fstat/lstat differ\n", what); fails++;
	}
}

int main(void)
{
	char q[256];
	const char *B = "/tmp/fdpath";
	mkdir(B, 0700); mkdir("/tmp/fdpath/real", 0700);
	unlink("/tmp/fdpath/abs"); unlink("/tmp/fdpath/rel");
	unlink("/tmp/fdpath/real/f"); unlink("/tmp/fdpath/real/g"); unlink("/tmp/fdpath/real/sub/x"); rmdir("/tmp/fdpath/real/sub");
	if (symlink("/tmp/fdpath/real", "/tmp/fdpath/abs") || symlink("../fdpath/real", "/tmp/fdpath/rel")) {
		printf("FAIL symlink errno=%d\n", errno); return 1;
	}

	int d = open("/tmp/fdpath/real", O_PATH | O_DIRECTORY | O_CLOEXEC);
	want("plain dir", d, "/tmp/fdpath/real");

	/* mkdir and open a child through the descriptor, as Claude Code does */
	snprintf(q, sizeof q, "/proc/self/fd/%d/sub", d);
	if (mkdir(q, 0700) != 0) { printf("FAIL mkdir via fd errno=%d\n", errno); fails++; }
	int s = open(q, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	want("child via /proc/self/fd", s, "/tmp/fdpath/real/sub");
	snprintf(q, sizeof q, "/proc/self/fd/%d/x", s);
	int x = open(q, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	want("create via /proc/self/fd", x, "/tmp/fdpath/real/sub/x");

	int f = open("/tmp/fdpath/abs/f", O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
	want("create through absolute symlink", f, "/tmp/fdpath/real/f");
	int r = open("/tmp/fdpath/rel/f", O_RDONLY | O_CLOEXEC);
	want("open through relative symlink", r, "/tmp/fdpath/real/f");
	int dd = open("/tmp/fdpath/rel/sub/../.", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	want("dotdot after a symlink", dd, "/tmp/fdpath/real");
	int la = open("/tmp/fdpath/abs", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	int g = openat(la, "g", O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
	want("openat on a dir opened via symlink", g, "/tmp/fdpath/real/g");
	int root = open("/", O_RDONLY | O_DIRECTORY);
	want("root", root, "/");

	if (fails == 0) printf("OK\n");
	return fails != 0;
}
