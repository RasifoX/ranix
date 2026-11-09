/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* kcmp(2) tells whether two processes share a kernel object: 0 = same,
 * 1 or 2 = different (an order), 3 = not comparable. It reveals another
 * process's layout, so it needs the same right as ptrace attach: a user may
 * not compare a root process. Checks KCMP_FILE (dup shares, a second open
 * does not, fork shares), KCMP_VM, KCMP_FILES, KCMP_SIGHAND, bad fd EBADF,
 * bad type EINVAL, and the permission check.
 * Prints FAIL lines, then OK or FAIL <n>. Runs as root (a child drops). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#ifndef SYS_kcmp
#define SYS_kcmp 312
#endif
#define KCMP_FILE    0
#define KCMP_VM      1
#define KCMP_FILES   2
#define KCMP_FS      3
#define KCMP_SIGHAND 4

#define USER 4321

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (errno=%d)\n", what, errno); fails++; } } while (0)

static long kcmp(pid_t a, pid_t b, int type, long x, long y)
{
	return syscall(SYS_kcmp, a, b, type, x, y);
}

int main(void)
{
	if (geteuid() != 0) { puts("FAIL run as root"); return 0; }
	pid_t me = getpid();
	int fd = open("/proc/self/stat", O_RDONLY);
	int d = dup(fd);
	int other = open("/proc/self/stat", O_RDONLY);
	if (fd < 0 || d < 0 || other < 0) { puts("FAIL setup fds"); return 0; }

	CHECK(kcmp(me, me, KCMP_FILE, fd, d) == 0, "KCMP_FILE: dup is not the same file");
	long r = kcmp(me, me, KCMP_FILE, fd, other);
	CHECK(r == 1 || r == 2, "KCMP_FILE: a second open is the same file");
	errno = 0;
	CHECK(kcmp(me, me, KCMP_FILE, fd, 999) == -1 && errno == EBADF, "KCMP_FILE: bad fd");
	errno = 0;
	CHECK(kcmp(me, me, 99, 0, 0) == -1 && errno == EINVAL, "unknown kcmp type");
	CHECK(kcmp(me, me, KCMP_VM, 0, 0) == 0, "KCMP_VM: self differs from self");

	int go[2];
	if (pipe(go)) { puts("FAIL pipe"); return 0; }
	pid_t c = fork();
	if (c == 0) { char b; close(go[1]); read(go[0], &b, 1); _exit(0); }
	close(go[0]);
	CHECK(kcmp(me, c, KCMP_FILE, fd, fd) == 0, "KCMP_FILE: fork does not share the open file");
	r = kcmp(me, c, KCMP_VM, 0, 0);
	CHECK(r == 1 || r == 2, "KCMP_VM: a forked child shares the address space");
	r = kcmp(me, c, KCMP_FILES, 0, 0);
	CHECK(r == 1 || r == 2, "KCMP_FILES: a forked child shares the fd table");
	r = kcmp(me, c, KCMP_SIGHAND, 0, 0);
	CHECK(r == 1 || r == 2, "KCMP_SIGHAND: a forked child shares the handler table");
	r = kcmp(me, c, KCMP_FS, 0, 0);
	if (!(r == 1 || r == 2)) printf("NOTE KCMP_FS not supported (r=%ld errno=%d)\n", r, errno);
	close(go[1]);
	waitpid(c, NULL, 0);

	/* a user comparing a root process: EPERM */
	fflush(stdout);
	c = fork();
	if (c == 0) {
		if (setgroups(0, NULL) || setgid(USER) || setuid(USER)) _exit(2);
		errno = 0;
		long x = kcmp(getpid(), me, KCMP_VM, 0, 0);
		_exit(x == -1 && errno == EPERM ? 0 : 1);
	}
	int st = 0;
	waitpid(c, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a user may kcmp a root process");

	if (fails) printf("FAIL %d\n", fails); else puts("OK");
	return 0;
}
