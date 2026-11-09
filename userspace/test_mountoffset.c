/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* mount(2) with offset=BYTES reaches a filesystem further into a device. Bad
 * offsets are refused, and a write lands inside the slice. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 5) { puts("FAIL usage: dev dir offset marker"); return 1; }
	const char *dev = argv[1], *dir = argv[2], *off = argv[3], *want = argv[4];
	char opt[32], got[64] = { 0 }, path[128];

	mkdir(dir, 0755);
	if (mount(dev, dir, "ext2", 0, "offset=1234") == 0 || errno != EINVAL) { puts("FAIL unaligned"); return 1; }
	if (mount(dev, dir, "ext2", 0, "offset=99999999999999") == 0 || errno != EINVAL) { puts("FAIL past end"); return 1; }
	snprintf(opt, sizeof(opt), "offset=%s", off);
	if (mount(dev, dir, "ext2", 0, opt) != 0) { printf("FAIL mount errno=%d\n", errno); return 1; }

	if (strcmp(want, "-") == 0) { puts("OK"); return 0; }   /* leave it mounted */

	snprintf(path, sizeof(path), "%s/marker.txt", dir);
	int fd = open(path, O_RDONLY);
	if (fd < 0 || read(fd, got, sizeof(got) - 1) <= 0) { puts("FAIL read"); return 1; }
	close(fd);
	if (strcmp(got, want) != 0) { printf("FAIL marker '%s'\n", got); return 1; }

	snprintf(path, sizeof(path), "%s/w.txt", dir);
	fd = open(path, O_CREAT | O_WRONLY, 0644);
	if (fd < 0 || write(fd, "x", 1) != 1) { puts("FAIL write"); return 1; }
	close(fd);
	if (umount(dir) != 0) { printf("FAIL umount errno=%d\n", errno); return 1; }
	puts("OK");
	return 0;
}
