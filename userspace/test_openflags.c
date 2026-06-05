/* open(2) flag enforcement:
 *  O_CREAT|O_EXCL on an existing file -> EEXIST
 *  O_DIRECTORY on a regular file      -> ENOTDIR
 *  O_WRONLY/O_RDWR on a directory      -> EISDIR
 *  O_CLOEXEC sets close-on-exec         */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int main(void)
{
	const char *f = "/tmp/of_file";
	int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) { printf("openflags-fail create\n"); return 0; }
	write(fd, "x", 1);
	close(fd);

	errno = 0;
	if (open(f, O_WRONLY | O_CREAT | O_EXCL, 0644) != -1 || errno != EEXIST) {
		printf("openflags-fail excl errno=%d\n", errno); return 0;
	}

	errno = 0;
	if (open(f, O_RDONLY | O_DIRECTORY) != -1 || errno != ENOTDIR) {
		printf("openflags-fail directory errno=%d\n", errno); return 0;
	}

	errno = 0;
	if (open("/tmp", O_WRONLY) != -1 || errno != EISDIR) {
		printf("openflags-fail isdir errno=%d\n", errno); return 0;
	}

	fd = open(f, O_RDONLY | O_CLOEXEC);
	if (fd < 0) { printf("openflags-fail cloexec-open\n"); return 0; }
	int fl = fcntl(fd, F_GETFD);
	if (fl < 0 || !(fl & FD_CLOEXEC)) { printf("openflags-fail cloexec-bit\n"); return 0; }
	close(fd);

	/* opening a directory read-only must still work (opendir relies on it) */
	fd = open("/tmp", O_RDONLY | O_DIRECTORY);
	if (fd < 0) { printf("openflags-fail diropen\n"); return 0; }
	close(fd);

	printf("OK\n");
	return 0;
}
