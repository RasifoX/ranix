/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* On a read-only mount, creating what already exists is EEXIST, not EROFS, while
 * creating something new is EROFS. BusyBox mkdir -p depends on the first. */
#include <errno.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
	mkdir("/tmp/roe", 0755);
	if (mount("none", "/tmp/roe", "tmpfs", 0, NULL) != 0) { perror("mount"); return 1; }
	mkdir("/tmp/roe/d", 0755);
	if (mount("none", "/tmp/roe", "tmpfs", MS_REMOUNT | MS_RDONLY, NULL) != 0) { perror("remount"); return 1; }
	int r = mkdir("/tmp/roe/d", 0755);
	int e = errno;
	int r2 = mkdir("/tmp/roe/new", 0755);
	int e2 = errno;
	umount("/tmp/roe");
	if (r != -1 || e != EEXIST) { printf("FAIL existing: r=%d errno=%d\n", r, e); return 1; }
	if (r2 != -1 || e2 != EROFS) { printf("FAIL new: r=%d errno=%d\n", r2, e2); return 1; }
	printf("OK\n");
	return 0;
}
