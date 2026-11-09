/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A path that cannot be walked says why, as on Linux: a file used as a
 * directory is ENOTDIR, a symlink loop is ELOOP, a name over 255 bytes is
 * ENAMETOOLONG; only a missing name is ENOENT. A chain of 30 symlinks still
 * resolves (Linux follows 40). LTP access04, chdir04, chmod06, chown04 and
 * chroot03 test exactly these. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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
	struct stat st;
	char longname[300], longpath[320], a[64], b[64];

	mkdir("/tmp/pe", 0755);
	close(open("/tmp/pe/file", O_CREAT | O_WRONLY, 0644));
	symlink("/tmp/pe/loop2", "/tmp/pe/loop1");
	symlink("/tmp/pe/loop1", "/tmp/pe/loop2");
	symlink("/tmp/pe/file", "/tmp/pe/c0");
	for (int i = 1; i <= 30; i++) {
		snprintf(a, sizeof a, "/tmp/pe/c%d", i - 1);
		snprintf(b, sizeof b, "/tmp/pe/c%d", i);
		symlink(a, b);
	}
	memset(longname, 'x', 299); longname[299] = '\0';
	snprintf(longpath, sizeof longpath, "/tmp/pe/%s", longname);

	errno = 0; want("access file/x", access("/tmp/pe/file/x", F_OK), ENOTDIR);
	errno = 0; want("stat file/x", stat("/tmp/pe/file/x", &st), ENOTDIR);
	errno = 0; want("chdir file/x", chdir("/tmp/pe/file/x"), ENOTDIR);
	errno = 0; want("chmod file/x", chmod("/tmp/pe/file/x", 0600), ENOTDIR);
	errno = 0; want("open O_CREAT file/x", open("/tmp/pe/file/x", O_CREAT | O_WRONLY, 0644), ENOTDIR);
	errno = 0; want("stat loop", stat("/tmp/pe/loop1", &st), ELOOP);
	errno = 0; want("access loop/x", access("/tmp/pe/loop1/x", F_OK), ELOOP);
	errno = 0; want("open loop", open("/tmp/pe/loop1", O_RDONLY), ELOOP);
	errno = 0; want("stat 30 links", stat("/tmp/pe/c30", &st), 0);
	errno = 0; want("access long", access(longpath, F_OK), ENAMETOOLONG);
	errno = 0; want("stat missing/x", stat("/tmp/pe/none/x", &st), ENOENT);
	errno = 0; want("stat missing", stat("/tmp/pe/none", &st), ENOENT);
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
