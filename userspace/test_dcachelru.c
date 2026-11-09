/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The dentry cache stays bounded under a find-style walk, and dropping cached
 * names breaks nothing. Past vfs_dentry_max the oldest unheld disk-filesystem
 * dentries are dropped; a walk over more files than the limit must not grow the
 * cache by more than a fraction of them, and whatever a task holds (its root, an
 * open file, a mountpoint) must survive.
 *
 *   test_dcachelru DEV MNT      DEV ext2 at MNT (the suite's nvme /mnt2)
 *
 * Builds MNT/lru: 40 directories of 100 empty files, a relative link in each.
 *   bounded     a walk of every name grows the cache by under a quarter of them
 *               (only when the limit is in reach; reported as "no pressure" else)
 *   same-inodes a second walk finds every name with the inode number of the first
 *   links       every MNT/lru/dN/l -> ../d0/f0 still resolves
 *   chroot      a child chrooted at MNT/lru/d1 still finds /f1 and "/.." is "/"
 *   open-file   a file kept open keeps its contents and its name
 *   mountpoint  a tmpfs mounted on MNT/lru/mp is still there
 *   chmod-held  after chmod 700 MNT/lru/d3 and the walks, nobody gets EACCES
 * Prints one PASS/FAIL line a case, then OK or FAIL. Runs as root. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define NDIR 40
#define NFILE 100
#define NOBODY 65534

static const char *g_mnt;
static int g_fail;
static ino_t g_ino[NDIR][NFILE];

static void result(const char *name, int ok, const char *why)
{
	printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, ok ? "" : ": ", ok ? "" : why);
	if (!ok) g_fail = 1;
}

static const char *P(const char *fmt, int a, int b)
{
	static char buf[4][300];
	static int i;
	char rel[64];
	i = (i + 1) & 3;
	snprintf(rel, sizeof rel, fmt, a, b);
	snprintf(buf[i], sizeof buf[i], "%s/lru%s", g_mnt, rel);
	return buf[i];
}

static long dentries(void)
{
	char b[64] = "";
	int fd = open("/proc/sys/fs/dentry-state", O_RDONLY);
	if (fd < 0) return -1;
	ssize_t n = read(fd, b, sizeof b - 1);
	close(fd);
	return n > 0 ? atol(b) : -1;
}

static long limit(void)
{
	char b[1024] = "";
	int fd = open("/proc/cmdline", O_RDONLY);
	if (fd >= 0) { if (read(fd, b, sizeof b - 1) < 0) { } close(fd); }
	char *p = strstr(b, "vfs_dentry_max=");
	return p ? atol(p + 15) : 131072;
}

/* stat every file; record (first) or compare inode numbers; returns the peak count */
static long walk(int record, int *mismatch)
{
	long peak = dentries();
	struct stat st;
	for (int d = 0; d < NDIR; d++)
		for (int f = 0; f < NFILE; f++) {
			if (stat(P("/d%d/f%d", d, f), &st) != 0) { (*mismatch)++; continue; }
			if (record) g_ino[d][f] = st.st_ino;
			else if (g_ino[d][f] != st.st_ino) (*mismatch)++;
			if (f % 20 == 0) { long n = dentries(); if (n > peak) peak = n; }
		}
	return peak;
}

static int nobody_reads(const char *path)
{
	pid_t c = fork();
	if (c == 0) {
		if (setgid(NOBODY) || setuid(NOBODY)) _exit(9);
		int fd = open(path, O_RDONLY);
		_exit(fd >= 0 ? 0 : (errno == EACCES ? 1 : 2));
	}
	int st = 0;
	waitpid(c, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 3;
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: test_dcachelru DEV MNT\n"); return 2; }
	g_mnt = argv[2];
	setvbuf(stdout, NULL, _IONBF, 0);
	char cmd[300];
	snprintf(cmd, sizeof cmd, "rm -rf %s/lru", g_mnt);
	if (system(cmd)) { }
	mkdir(P("", 0, 0), 0755);
	mkdir(P("/mp", 0, 0), 0755);
	for (int d = 0; d < NDIR; d++) {
		mkdir(P("/d%d", d, 0), 0755);
		for (int f = 0; f < NFILE; f++) {
			int fd = open(P("/d%d/f%d", d, f), O_WRONLY | O_CREAT, 0644);
			if (fd < 0) { printf("FAIL setup: create d%d/f%d errno %d\nFAIL\n", d, f, errno); return 1; }
			close(fd);
		}
		if (symlink("../d0/f0", P("/d%d/l", d, 0))) { }
	}
	sync();
	if (umount(g_mnt) || mount(argv[1], g_mnt, "ext2", 0, NULL)) {   /* nothing below MNT cached */
		printf("FAIL setup: mount again, errno %d\nFAIL\n", errno);
		return 1;
	}

	/* held things, set up before the walks push names out */
	int of = open(P("/d2/f2", 0, 0), O_RDWR);
	if (of < 0 || write(of, "x", 1) != 1) { printf("FAIL setup: open d2/f2\nFAIL\n"); return 1; }
	int go[2], back[2];
	if (pipe(go) || pipe(back)) return 1;
	pid_t ch = fork();
	if (ch == 0) {
		char c = chroot(P("/d1", 0, 0)) == 0 && chdir("/") == 0 ? '1' : '0';
		if (write(back[1], &c, 1) != 1 || read(go[0], &c, 1) != 1) _exit(1);
		struct stat a, b, r;
		c = stat("/f1", &a) == 0 && stat("/", &r) == 0 && stat("/..", &b) == 0 &&
		    r.st_ino == b.st_ino && r.st_dev == b.st_dev ? '1' : '0';
		if (write(back[1], &c, 1) != 1) _exit(1);
		_exit(0);
	}
	char c = 0;
	if (read(back[0], &c, 1) != 1 || c != '1') { printf("FAIL setup: chroot\nFAIL\n"); return 1; }
	int mounted = mount("none", P("/mp", 0, 0), "tmpfs", 0, NULL) == 0;
	int mp_fd = open(P("/mp/here", 0, 0), O_WRONLY | O_CREAT, 0644);
	if (mp_fd >= 0) close(mp_fd);
	chmod(P("/d3", 0, 0), 0700);

	long max = limit(), c0 = dentries(), mism = 0;
	int m1 = 0, m2 = 0;
	long peak = walk(1, &m1);
	long peak2 = walk(0, &m2);
	if (peak2 > peak) peak = peak2;
	mism = m1 + m2;
	long n = (long)NDIR * NFILE, grew = peak - c0;
	char why[200];
	if (c0 + n > max) {
		snprintf(why, sizeof why, "cache grew by %ld for %ld names (start %ld, limit %ld)", grew, n, c0, max);
		result("bounded", grew < n / 4, why);
	} else {
		printf("PASS bounded: no pressure (start %ld + %ld names <= limit %ld), grew %ld\n", c0, n, max, grew);
	}
	snprintf(why, sizeof why, "%ld names missing or with another inode", mism);
	result("same-inodes", mism == 0, why);

	int bad = 0;
	char b[8];
	for (int d = 0; d < NDIR; d++) {
		int fd = open(P("/d%d/l", d, 0), O_RDONLY);
		if (fd < 0) bad++; else close(fd);
	}
	snprintf(why, sizeof why, "%d of %d links did not resolve", bad, NDIR);
	result("links", bad == 0, why);

	if (write(go[1], "g", 1) != 1) { }
	c = 0;
	int croot = read(back[0], &c, 1) == 1 && c == '1';
	waitpid(ch, NULL, 0);
	result("chroot", croot, "chrooted child lost /f1 or /.. left the root");

	struct stat a, s2;
	int okf = pread(of, b, 1, 0) == 1 && b[0] == 'x' && fstat(of, &a) == 0 &&
	          stat(P("/d2/f2", 0, 0), &s2) == 0 && a.st_ino == s2.st_ino && s2.st_size == 1;
	close(of);
	result("open-file", okf, "the open file lost its contents or its name");

	result("mountpoint", mounted && access(P("/mp/here", 0, 0), F_OK) == 0, "the tmpfs on MNT/lru/mp is gone");
	umount(P("/mp", 0, 0));

	struct stat d3;
	int r3 = nobody_reads(P("/d3/f3", 0, 0));
	result("chmod-held", stat(P("/d3", 0, 0), &d3) == 0 && (d3.st_mode & 0777) == 0700 && r3 == 1,
	       "chmod 700 did not hold after the walks");

	if (system(cmd)) { }
	printf("%s\n", g_fail ? "FAIL" : "OK");
	return g_fail;
}
