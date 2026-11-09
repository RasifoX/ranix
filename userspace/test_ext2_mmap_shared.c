/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* test_ext2_mmap_shared DEV DIR OFFSET BSIZE
 * Stores through a MAP_SHARED mapping reach the file on an ext2 whose block is
 * smaller than a page, as on Linux. Mounts the ext2 at OFFSET bytes into DEV
 * on DIR (block size BSIZE) and checks:
 * - after msync(MS_SYNC), read() sees the store;
 * - after munmap alone, and after close then munmap, read() sees it;
 * - a child that stores and exits without munmap: the parent reads it;
 * - a child that stores into a mapping inherited across fork: the parent
 *   sees it in its own mapping (MAP_SHARED is shared) and in the file;
 * - a write() to other bytes of the file while it is mapped survives the
 *   mapping's write-back (only the bytes the mapping changed go back);
 * - a store past EOF inside the last page does not grow the file;
 * - a MAP_PRIVATE store does not reach the file;
 * - after umount and a fresh mount everything above is on the disk.
 * DEV "-" uses DIR as it is (no mount): the same checks against any Linux
 * filesystem, as a reference. Prints OK, or FAIL lines. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>

#define PG    4096
#define FLEN  (3 * PG + 1500)   /* the last page is partly past EOF */

static int fails;
#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } while (0)

static char path[256];

/* byte at off, as read() sees it */
static int byte_at(off_t off)
{
	int fd = open(path, O_RDONLY);
	unsigned char c = 0;
	int r = (fd >= 0 && pread(fd, &c, 1, off) == 1) ? c : -1;
	if (fd >= 0) close(fd);
	return r;
}

static unsigned char *map_shared(int fd)
{
	unsigned char *m = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) { FAIL("mmap shared errno %d", errno); return NULL; }
	return m;
}

/* the stores the checks below make, and where */
struct mark { off_t off; unsigned char v; const char *what; };
static const struct mark marks[] = {
	{ 10,          0xa1, "msync" },
	{ PG + 20,     0xa2, "munmap" },
	{ 2 * PG + 30, 0xa3, "close then munmap" },
	{ 2 * PG + 40, 0xa4, "child exit" },
	{ 3 * PG + 50, 0xa5, "child after fork" },
	{ 70,          0xa6, "store next to a write()" },
	{ 100,         0xb7, "write() while mapped" },
};

static void stores(void)
{
	int fd = open(path, O_RDWR);
	unsigned char *m = map_shared(fd);
	if (!m) return;
	m[10] = 0xa1;
	msync(m, 4 * PG, MS_SYNC);
	if (byte_at(10) != 0xa1) FAIL("after msync read() sees %#x, want 0xa1", byte_at(10));
	m[PG + 20] = 0xa2;
	munmap(m, 4 * PG);
	if (byte_at(PG + 20) != 0xa2) FAIL("after munmap read() sees %#x, want 0xa2", byte_at(PG + 20));

	m = map_shared(fd);
	if (!m) return;
	close(fd);
	m[2 * PG + 30] = 0xa3;
	munmap(m, 4 * PG);
	if (byte_at(2 * PG + 30) != 0xa3) FAIL("after close+munmap read() sees %#x, want 0xa3", byte_at(2 * PG + 30));

	pid_t c = fork();
	if (c == 0) {
		int cfd = open(path, O_RDWR);
		unsigned char *cm = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
		if (cm != MAP_FAILED) cm[2 * PG + 40] = 0xa4;
		_exit(0);   /* no munmap, no msync */
	}
	waitpid(c, NULL, 0);
	if (byte_at(2 * PG + 40) != 0xa4) FAIL("after the child's exit read() sees %#x, want 0xa4", byte_at(2 * PG + 40));

	fd = open(path, O_RDWR);
	m = map_shared(fd);
	if (!m) return;
	c = fork();
	if (c == 0) { m[3 * PG + 50] = 0xa5; _exit(0); }
	waitpid(c, NULL, 0);
	if (m[3 * PG + 50] != 0xa5) FAIL("the child's store is not in the parent's mapping");
	unsigned char w = 0xb7;
	if (pwrite(fd, &w, 1, 100) != 1) FAIL("pwrite errno %d", errno);
	m[70] = 0xa6;
	m[FLEN + 7] = 0xee;   /* past EOF */
	munmap(m, 4 * PG);
	close(fd);
	for (size_t i = 3; i < sizeof(marks) / sizeof(marks[0]); i++)
		if (byte_at(marks[i].off) != marks[i].v)
			FAIL("%s: read() sees %#x at %ld, want %#x", marks[i].what,
			     byte_at(marks[i].off), (long)marks[i].off, marks[i].v);
	struct stat st;
	if (stat(path, &st) || st.st_size != FLEN) FAIL("size %ld after a store past EOF, want %d", (long)st.st_size, FLEN);

	fd = open(path, O_RDWR);
	m = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (m != MAP_FAILED) { m[200] = 0x99; msync(m, 4 * PG, MS_SYNC); munmap(m, 4 * PG); }
	close(fd);
	if (byte_at(200) == 0x99) FAIL("a MAP_PRIVATE store reached the file");
}

static void on_disk(void)
{
	for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++)
		if (byte_at(marks[i].off) != marks[i].v)
			FAIL("after remount, %s: %#x at %ld, want %#x", marks[i].what,
			     byte_at(marks[i].off), (long)marks[i].off, marks[i].v);
}

int main(int argc, char **argv)
{
	if (argc < 5) { puts("FAIL: usage DEV DIR OFFSET BSIZE"); return 1; }
	const char *dev = argv[1], *dir = argv[2];
	long want = atol(argv[4]);
	char opt[48];
	snprintf(opt, sizeof(opt), "offset=%s", argv[3]);
	int mnt = strcmp(dev, "-") != 0;
	mkdir(dir, 0755);
	if (mnt && mount(dev, dir, "ext2", 0, opt)) { printf("FAIL: mount errno %d\n", errno); return 1; }
	struct statfs sf;
	if (mnt && (statfs(dir, &sf) || sf.f_bsize != want))
		FAIL("block size %ld, want %ld", (long)sf.f_bsize, want);
	snprintf(path, sizeof(path), "%s/shared", dir);
	unlink(path);
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	char *z = calloc(1, FLEN);
	if (fd < 0 || !z || write(fd, z, FLEN) != FLEN) FAIL("create errno %d", errno);
	free(z);
	close(fd);
	stores();
	if (mnt) {
		if (umount(dir)) FAIL("umount errno %d", errno);
		else if (mount(dev, dir, "ext2", 0, opt)) FAIL("remount errno %d", errno);
		else { on_disk(); unlink(path); if (umount(dir)) FAIL("second umount errno %d", errno); }
	} else {
		on_disk();
		unlink(path);
	}
	if (!fails) puts("OK");
	return fails != 0;
}
