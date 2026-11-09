/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* stat() through /proc/self/fd/N reaches the open file even when it has no
 * path (a pipe, a socket): Chromium's sandbox walks /proc/self/fd with
 * fstatat() and CHECKs every entry ("Check failed: . : No such file"). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>

int main(void)
{
	int p[2], sv[2];
	char path[64], name[16];
	struct stat a, b;
	if (pipe(p) || socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { printf("FAIL setup\n"); return 0; }
	snprintf(path, sizeof path, "/proc/self/fd/%d", p[0]);
	if (stat(path, &a) != 0 || !S_ISFIFO(a.st_mode)) { printf("FAIL pipe stat\n"); return 0; }
	if (fstat(p[0], &b) != 0 || a.st_ino != b.st_ino) { printf("FAIL pipe ino\n"); return 0; }
	int d = open("/proc/self/fd/", O_DIRECTORY | O_RDONLY);
	snprintf(name, sizeof name, "%d", sv[0]);
	if (d < 0 || fstatat(d, name, &a, 0) != 0 || !S_ISSOCK(a.st_mode)) { printf("FAIL socket fstatat\n"); return 0; }
	if (lstat(path, &a) != 0 || !S_ISLNK(a.st_mode)) { printf("FAIL lstat is not the link\n"); return 0; }
	printf("OK\n");
	return 0;
}
