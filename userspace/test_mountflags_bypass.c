/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Mount flags follow the mount a path really reaches, however it is spelled:
 * through a symlink, "..", /proc/self/cwd or a chroot. The string prefix of the
 * mountpoint used to decide this, and none of those spellings match it. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define T "/tmp/mfb"

static int bad;

static void want(const char *what, int got, int exp)
{
	if (got == exp) return;
	printf("FAIL %s: got %d want %d\n", what, got, exp);
	bad = 1;
}

/* errno of open(), 0 on success */
static int try_open(const char *p, int fl)
{
	int fd = open(p, fl, 0644);
	if (fd < 0) return errno;
	close(fd);
	return 0;
}

static int rc(int r) { return r ? errno : 0; }

static void file(const char *p)
{
	int fd = open(p, O_WRONLY | O_CREAT, 0755);
	if (fd >= 0) { write(fd, "x", 1); close(fd); }
}

static int copy(const char *src, const char *dst)
{
	int in = open(src, O_RDONLY), out = open(dst, O_WRONLY | O_CREAT, 0755);
	char b[4096]; ssize_t n;
	if (in < 0 || out < 0) return -1;
	while ((n = read(in, b, sizeof(b))) > 0) write(out, b, n);
	close(in); close(out);
	return 0;
}

static int exec_errno(const char *p)
{
	int e[2];
	pipe2(e, O_CLOEXEC);
	pid_t c = fork();
	if (c == 0) {
		char *av[] = { "echo", NULL };
		int dn = open("/dev/null", O_WRONLY);
		dup2(dn, 1);
		execv(p, av);
		int er = errno;
		write(e[1], &er, sizeof(er));
		_exit(0);
	}
	close(e[1]);
	int er = 0;
	read(e[0], &er, sizeof(er));
	close(e[0]);
	waitpid(c, NULL, 0);
	return er;
}

int main(void)
{
	mkdir("/tmp", 0755);
	mkdir(T, 0755);
	mkdir(T "/ro", 0755);
	mkdir(T "/rw", 0755);
	mkdir(T "/nx", 0755);
	mkdir(T "/plain", 0755);
	if (mount("none", T "/ro", "tmpfs", 0, NULL)) { puts("FAIL mount ro"); return 1; }
	if (mount("none", T "/nx", "tmpfs", 0, NULL)) { puts("FAIL mount nx"); return 1; }
	mkdir(T "/ro/sub", 0755);
	file(T "/ro/f");
	file(T "/ro/sub/g");
	file(T "/rw/f");
	if (symlink("../rw/f", T "/ro/out")) { puts("FAIL symlink out"); return 1; }
	copy("/bin/busybox", T "/nx/b");
	if (mount("none", T "/ro", "tmpfs", MS_REMOUNT | MS_RDONLY, NULL)) { puts("FAIL remount"); return 1; }
	if (mount("none", T "/nx", "tmpfs", MS_REMOUNT | MS_NOEXEC, NULL)) { puts("FAIL remount nx"); return 1; }
	symlink(T "/ro", T "/rolink");
	symlink(T "/ro/f", T "/flink");
	symlink(T "/ro/missing", T "/dangling");
	symlink(T "/nx", T "/nxlink");
	symlink(T "/nx/b", T "/blink");

	/* the plain spelling still works */
	want("plain ro", try_open(T "/ro/f", O_WRONLY), EROFS);
	want("plain ro read", try_open(T "/ro/f", O_RDONLY), 0);
	want("plain rw", try_open(T "/rw/f", O_WRONLY), 0);

	/* directory symlink, final symlink, dangling symlink */
	want("dir link", try_open(T "/rolink/f", O_WRONLY), EROFS);
	want("dir link deep", try_open(T "/rolink/sub/g", O_WRONLY), EROFS);
	want("dir link create", try_open(T "/rolink/new", O_WRONLY | O_CREAT), EROFS);
	want("final link", try_open(T "/flink", O_WRONLY), EROFS);
	want("dangling link", try_open(T "/dangling", O_WRONLY | O_CREAT), EROFS);
	want("link read", try_open(T "/rolink/f", O_RDONLY), 0);
	want("chmod via link", rc(chmod(T "/flink", 0600)), EROFS);
	want("chown via dir link", rc(chown(T "/rolink/f", 0, 0)), EROFS);
	want("mkdir via dir link", rc(mkdir(T "/rolink/d", 0755)), EROFS);
	want("unlink via dir link", rc(unlink(T "/rolink/f")), EROFS);
	want("rename into", rc(rename(T "/rw/f", T "/rolink/f2")), EROFS);
	want("symlink into", rc(symlink("x", T "/rolink/s")), EROFS);
	printf("symlink ok\n");

	/* a name in a read-only mount stays read-only whatever it points at */
	want("unlink ro link to rw", rc(unlink(T "/ro/out")), EROFS);
	want("lchown ro link to rw", rc(lchown(T "/ro/out", 0, 0)), EROFS);

	/* ".." and /proc/self/cwd */
	want("dotdot", try_open(T "/rw/../ro/f", O_WRONLY), EROFS);
	want("dotdot in", try_open(T "/ro/sub/../f", O_WRONLY), EROFS);
	want("dotdot out", try_open(T "/ro/../rw/f", O_WRONLY), 0);
	chdir(T "/ro");
	want("cwd", try_open("f", O_WRONLY), EROFS);
	want("proc cwd", try_open("/proc/self/cwd/f", O_WRONLY), EROFS);
	chdir("/");
	int fd = open(T "/ro", O_RDONLY | O_DIRECTORY);
	char p[64];
	snprintf(p, sizeof(p), "/proc/self/fd/%d/f", fd);
	want("proc fd", try_open(p, O_WRONLY), EROFS);
	close(fd);
	printf("spelling ok\n");

	/* noexec */
	want("exec plain", exec_errno(T "/nx/b"), EACCES);
	want("exec dir link", exec_errno(T "/nxlink/b"), EACCES);
	want("exec final link", exec_errno(T "/blink"), EACCES);
	printf("noexec ok\n");

	/* inside a chroot the mountpoint's global path is not a prefix of anything */
	pid_t c = fork();
	if (c == 0) {
		if (chroot(T)) _exit(1);
		_exit(try_open("/ro/f", O_WRONLY) == EROFS &&
		      try_open("/ro/new", O_WRONLY | O_CREAT) == EROFS &&
		      try_open("/rw/f", O_WRONLY) == 0 ? 0 : 2);
	}
	int st;
	waitpid(c, &st, 0);
	want("chroot", WEXITSTATUS(st), 0);
	printf("chroot ok\n");

	umount(T "/ro");
	umount(T "/nx");
	if (!bad) printf("OK\n");
	return bad;
}
