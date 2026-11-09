/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Making a name under a regular file is ENOTDIR, for every call that makes or
 * removes one, and leaves the file as it was. A filesystem's create once took
 * the file for a directory and wrote a list entry into its data index; evicting
 * the file then crashed the kernel (LTP bind01, creat06 and others). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int bad;

static void want(const char *what, int r, int err)
{
	int got = r < 0 ? errno : 0;
	if (got != err) {
		printf("%s: want %s, got %s\n", what, strerror(err), got ? strerror(got) : "success");
		bad++;
	}
}

int main(void)
{
	const char *F = "/tmp/ndp-file";
	char buf[8] = { 0 };
	int fd = open(F, O_CREAT | O_RDWR | O_TRUNC, 0644);
	write(fd, "abc", 3);
	close(fd);
	close(open("/tmp/ndp-other", O_CREAT | O_WRONLY, 0644));

	errno = 0; want("open O_CREAT", open("/tmp/ndp-file/x", O_CREAT | O_WRONLY, 0644), ENOTDIR);
	errno = 0; want("mkdir", mkdir("/tmp/ndp-file/x", 0755), ENOTDIR);
	errno = 0; want("mkfifo", mkfifo("/tmp/ndp-file/x", 0644), ENOTDIR);
	errno = 0; want("symlink", symlink("/tmp", "/tmp/ndp-file/x"), ENOTDIR);
	errno = 0; want("link", link("/tmp/ndp-other", "/tmp/ndp-file/x"), ENOTDIR);
	errno = 0; want("rename", rename("/tmp/ndp-other", "/tmp/ndp-file/x"), ENOTDIR);
	errno = 0; want("unlink", unlink("/tmp/ndp-file/x"), ENOTDIR);
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	strcpy(a.sun_path, "/tmp/ndp-file/sock");
	errno = 0; want("bind", bind(s, (struct sockaddr *)&a, sizeof a), ENOTDIR);
	close(s);

	fd = open(F, O_RDONLY);
	if (read(fd, buf, sizeof buf) != 3 || strcmp(buf, "abc")) { printf("file changed: '%s'\n", buf); bad++; }
	close(fd);
	unlink(F);                 /* the eviction that crashed */
	unlink("/tmp/ndp-other");
	sync();
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
