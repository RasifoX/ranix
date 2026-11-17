/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Making a name inside a removed directory. A directory fd keeps a removed
 * directory readable, but nothing may be added to it: Linux answers ENOENT for
 * every create, link and rename into a dead directory. A name put there could
 * never be reached or removed, and its dentry would hang off one on its way to
 * the grave. Prints OK or FAIL lines. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BASE "/tmp/deaddir"

static int bad;

static void want_enoent(const char *what, int r)
{
	if (r == 0 || errno != ENOENT) {
		printf("FAIL: %s into a removed directory: r=%d %s, want ENOENT\n",
		       what, r, r == 0 ? "ok" : strerror(errno));
		bad++;
	}
}

int main(void)
{
	mkdir(BASE, 0755);
	mkdir(BASE "/a", 0755);
	mkdir(BASE "/dead", 0755);
	int fd = open(BASE "/a/x", O_CREAT | O_WRONLY, 0644);
	if (fd >= 0) close(fd);
	mkdir(BASE "/a/sub", 0755);

	int dfd = open(BASE "/dead", O_RDONLY | O_DIRECTORY);
	if (dfd < 0) { printf("FAIL: open dir: %s\n", strerror(errno)); return 1; }
	if (rmdir(BASE "/dead") != 0) { printf("FAIL: rmdir: %s\n", strerror(errno)); return 1; }

	int r = openat(dfd, "f", O_CREAT | O_WRONLY, 0644);
	if (r >= 0) close(r);
	want_enoent("openat O_CREAT", r >= 0 ? 0 : -1);
	want_enoent("mkdirat", mkdirat(dfd, "d", 0755));
	want_enoent("symlinkat", symlinkat("target", dfd, "l"));
	want_enoent("linkat", linkat(AT_FDCWD, BASE "/a/x", dfd, "h", 0));
	want_enoent("mknodat", mknodat(dfd, "p", S_IFIFO | 0644, 0));
	want_enoent("renameat file", renameat(AT_FDCWD, BASE "/a/x", dfd, "x"));
	want_enoent("renameat dir", renameat(AT_FDCWD, BASE "/a/sub", dfd, "sub"));

	/* the dead directory stays empty and readable through its fd */
	struct stat st;
	if (fstat(dfd, &st) != 0) { printf("FAIL: fstat: %s\n", strerror(errno)); bad++; }
	close(dfd);

	/* the sources are where they were, and the walk still finds them */
	if (stat(BASE "/a/x", &st) != 0) { printf("FAIL: a/x gone: %s\n", strerror(errno)); bad++; }
	if (stat(BASE "/a/sub", &st) != 0) { printf("FAIL: a/sub gone: %s\n", strerror(errno)); bad++; }

	unlink(BASE "/a/x");
	rmdir(BASE "/a/sub");
	rmdir(BASE "/a");
	rmdir(BASE);
	if (!bad) printf("OK\n");
	return bad != 0;
}
