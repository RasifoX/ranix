/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Metadata changes take one path: chmod, chown, utimens and xattr behave as before
 * through it, follow the owner rule, respect a read-only mount through an fd, and
 * on overlay reach the upper layer instead of a throwaway view inode. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>

#define T "/tmp/sat"
#define FAIL(what) do { printf("FAIL %s errno=%d\n", what, errno); return 1; } while (0)

static long mode_of(const char *p)
{
	struct stat st;
	return stat(p, &st) ? -1 : (long)(st.st_mode & 07777);
}

static int file(const char *p, mode_t m)
{
	int fd = open(p, O_WRONLY | O_CREAT, m);
	if (fd < 0) return -1;
	write(fd, "x", 1);
	close(fd);
	return chmod(p, m);
}

static int stranger(void)
{
	struct timespec ts[2] = { { 5, 0 }, { 6, 0 } };
	if (setuid(64004)) return 1;
	if (chmod(T "/f", 0666) != -1 || errno != EPERM) return 2;
	if (chown(T "/f", 64004, -1) != -1 || errno != EPERM) return 3;
	if (utimensat(AT_FDCWD, T "/f", ts, 0) != -1 || errno != EPERM) return 4;
	if (utimensat(AT_FDCWD, T "/f", NULL, 0) != -1 || errno != EACCES) return 5;
	int fd = open(T "/f", O_RDONLY);
	if (fd < 0) return 6;
	if (fchmod(fd, 0666) != -1 || errno != EPERM) return 7;
	close(fd);
	/* group-writable is enough for "now", never for a named time */
	if (utimensat(AT_FDCWD, T "/w", NULL, 0)) return 8;
	if (utimensat(AT_FDCWD, T "/w", ts, 0) != -1 || errno != EPERM) return 9;
	if (file(T "/mine", 0644)) return 10;
	if (chmod(T "/mine", 0600) || mode_of(T "/mine") != 0600) return 11;
	return 0;
}

static int run_stranger(void)
{
	pid_t c = fork();
	if (c == 0) _exit(stranger());
	int st;
	waitpid(c, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}

int main(void)
{
	struct stat st;
	char b[32];
	mkdir("/tmp", 0755);
	mkdir(T, 01777);
	chmod(T, 01777);
	if (file(T "/f", 0644)) FAIL("create");

	if (chmod(T "/f", 0640) || mode_of(T "/f") != 0640) FAIL("chmod");
	int fd = open(T "/f", O_RDONLY);
	if (fchmod(fd, 0600) || mode_of(T "/f") != 0600) FAIL("fchmod");
	close(fd);
	if (chmod(T "/f", 04755)) FAIL("chmod suid");
	if (chown(T "/f", 1, 2)) FAIL("chown");
	if (stat(T "/f", &st) || st.st_uid != 1 || st.st_gid != 2) FAIL("chown result");
	if (st.st_mode & 06000) FAIL("chown kept suid");
	if (chown(T "/f", -1, 3) || stat(T "/f", &st) || st.st_uid != 1 || st.st_gid != 3) FAIL("chown gid only");
	chown(T "/f", 0, 0);
	chmod(T "/f", 0644);

	struct timespec ts[2] = { { 1000, 11 }, { 2000, 22 } };
	if (utimensat(AT_FDCWD, T "/f", ts, 0)) FAIL("utimens");
	if (stat(T "/f", &st) || st.st_atim.tv_sec != 1000 || st.st_mtim.tv_sec != 2000) FAIL("utimens result");
	ts[0].tv_nsec = UTIME_OMIT;
	ts[1].tv_sec = 3000;
	if (utimensat(AT_FDCWD, T "/f", ts, 0)) FAIL("utimens omit");
	if (stat(T "/f", &st) || st.st_atim.tv_sec != 1000 || st.st_mtim.tv_sec != 3000) FAIL("omit result");
	fd = open(T "/f", O_RDONLY);
	if (futimens(fd, NULL) || fstat(fd, &st) || st.st_mtim.tv_sec == 3000) FAIL("futimens now");
	close(fd);

	if (setxattr(T "/f", "user.k", "v1", 2, 0)) FAIL("setxattr");
	if (getxattr(T "/f", "user.k", b, sizeof(b)) != 2 || memcmp(b, "v1", 2)) FAIL("getxattr");
	if (removexattr(T "/f", "user.k")) FAIL("removexattr");
	if (getxattr(T "/f", "user.k", b, sizeof(b)) != -1 || errno != ENODATA) FAIL("removed");
	printf("attrs ok\n");

	if (file(T "/w", 0666)) FAIL("create w");
	chmod(T "/f", 0644);
	int r = run_stranger();
	if (r) { printf("FAIL stranger rc=%d\n", r); return 1; }
	printf("owner ok\n");

	/* a read-only mount holds against an fd opened on it */
	mkdir(T "/ro", 0755);
	if (mount("none", T "/ro", "tmpfs", 0, NULL)) FAIL("mount");
	if (file(T "/ro/f", 0644)) FAIL("create ro");
	if (mount("none", T "/ro", "tmpfs", MS_REMOUNT | MS_RDONLY, NULL)) FAIL("remount");
	fd = open(T "/ro/f", O_RDONLY);
	if (fchmod(fd, 0600) != -1 || errno != EROFS) FAIL("fchmod ro");
	if (fchown(fd, 1, 1) != -1 || errno != EROFS) FAIL("fchown ro");
	if (futimens(fd, NULL) != -1 || errno != EROFS) FAIL("futimens ro");
	close(fd);
	umount(T "/ro");
	printf("rofs ok\n");

	/* overlay: chmod of a lower file copies it up and lasts */
	mkdir(T "/ov", 0755);
	if (mount("tmpfs", T "/ov", "tmpfs", 0, NULL)) FAIL("ov tmpfs");
	mkdir(T "/ov/lo", 0755); mkdir(T "/ov/up", 0755);
	mkdir(T "/ov/wo", 0755); mkdir(T "/ov/m", 0755);
	if (file(T "/ov/lo/low", 0644)) FAIL("lower file");
	if (mount("overlay", T "/ov/m", "overlay", 0,
	          "lowerdir=" T "/ov/lo,upperdir=" T "/ov/up,workdir=" T "/ov/wo")) FAIL("overlay");
	if (mode_of(T "/ov/m/low") != 0644) FAIL("overlay mode");
	if (chmod(T "/ov/m/low", 0600)) FAIL("overlay chmod");
	if (mode_of(T "/ov/m/low") != 0600) FAIL("overlay chmod lost");
	if (mode_of(T "/ov/lo/low") != 0644) FAIL("lower changed");
	if (mode_of(T "/ov/up/low") != 0600) FAIL("not in upper");
	if (chown(T "/ov/m/low", 7, 8) || stat(T "/ov/m/low", &st) || st.st_uid != 7) FAIL("overlay chown");
	ts[0] = (struct timespec){ 4000, 0 };
	ts[1] = (struct timespec){ 5000, 0 };
	if (utimensat(AT_FDCWD, T "/ov/m/low", ts, 0)) FAIL("overlay utimens");
	if (stat(T "/ov/m/low", &st) || st.st_mtim.tv_sec != 5000) FAIL("overlay utimens lost");
	umount(T "/ov/m");
	umount(T "/ov");
	printf("overlay ok\nOK\n");
	return 0;
}
