/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

/* fsync must flush a block device and report a bad descriptor. argv[1] names a
 * scratch block device; without it only the RAM paths are checked. */
int main(int argc, char **argv)
{
	char buf[512];
	if (fsync(-1) != -1 || errno != EBADF) { puts("FAIL ebadf"); return 1; }
	int fd = open("/tmp/fsync-test", O_CREAT | O_RDWR, 0600);
	if (fd < 0 || write(fd, "x", 1) != 1 || fsync(fd) != 0) { puts("FAIL ram"); return 1; }
	close(fd);
	if (argc > 1) {
		fd = open(argv[1], O_RDWR);
		if (fd < 0) { puts("FAIL open"); return 1; }
		memset(buf, 0x5a, sizeof(buf));
		if (lseek(fd, 1 << 20, SEEK_SET) < 0 || write(fd, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) { puts("FAIL write"); return 1; }
		if (fsync(fd) != 0) { puts("FAIL fsync"); return 1; }
		if (fsync(fd) != 0) { puts("FAIL fsync2"); return 1; }
		close(fd);
		sync();
	}
	puts("OK");
	return 0;
}
