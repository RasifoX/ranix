/* path canonicalization + readlink-on-non-symlink semantics. musl's userspace
 * realpath(3) canonicalizes by readlink()ing each accumulated prefix and
 * expecting EINVAL on non-symlinks — directories, regular files, and the "."
 * / ".." / "/" pseudo-components must all return EINVAL, not ENOENT. We also
 * check that opening via a messy path records the clean canonical path so
 * /proc/self/fd/N reports it. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

static int readlink_einval(const char *path)
{
	char b[64];
	errno = 0;
	return readlink(path, b, sizeof(b)) == -1 && errno == EINVAL;
}

int main(void)
{
	mkdir("/tmp/rp", 0755);
	mkdir("/tmp/rp/sub", 0755);
	int cfd = open("/tmp/rp/file", O_CREAT | O_WRONLY, 0644);
	if (cfd < 0) { printf("realpath-fail create\n"); return 0; }
	close(cfd);

	/* readlink on non-symlinks (dir, file, "/", ".", "..") → EINVAL */
	if (!readlink_einval("/tmp/rp/file") || !readlink_einval("/tmp/rp") ||
	    !readlink_einval("/") || !readlink_einval("/tmp/rp/.") ||
	    !readlink_einval("/tmp/rp/sub/..")) {
		printf("realpath-fail readlink-einval\n");
		return 0;
	}

	const char *messy = "/tmp/rp/./sub/../file";

	int fd = open(messy, O_RDONLY);
	if (fd < 0) { printf("realpath-fail open\n"); return 0; }
	char p[64], buf[256];
	snprintf(p, sizeof(p), "/proc/self/fd/%d", fd);
	long r = readlink(p, buf, sizeof(buf) - 1);
	int via_fd_ok = (r > 0 && (buf[r] = 0, strcmp(buf, "/tmp/rp/file") == 0));
	close(fd);

	char *rp = realpath(messy, NULL);
	int via_realpath_ok = (rp && strcmp(rp, "/tmp/rp/file") == 0);
	free(rp);

	printf("%s\n", (via_fd_ok && via_realpath_ok) ? "OK" : "realpath-fail canon");
	return 0;
}
