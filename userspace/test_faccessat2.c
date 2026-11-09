/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* access() checks the REAL ids; faccessat2(..., AT_EACCESS) checks the
 * effective ids. A setuid-root program (real uid = user, euid = 0) asks "may
 * the user who ran me read this?" with access() and "may I?" with AT_EACCESS.
 *  - faccessat2 must honour AT_EACCESS, AT_SYMLINK_NOFOLLOW and AT_EMPTY_PATH
 *    and refuse unknown flag bits with EINVAL;
 *  - the old faccessat syscall has only three arguments: whatever is in the
 *    fourth argument register must not change its answer.
 * Prints FAIL lines, then OK or FAIL <n>. Runs as root. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#ifndef SYS_faccessat2
#define SYS_faccessat2 439
#endif
#ifndef AT_EACCESS
#define AT_EACCESS 0x200
#endif

#define FILE_ "/tmp/facc2_file"
#define LINK_ "/tmp/facc2_dangling"
#define USER  4321

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (errno=%d)\n", what, errno); fails++; } } while (0)

static long facc2(const char *p, int mode, int flags)
{
	return syscall(SYS_faccessat2, AT_FDCWD, p, mode, flags);
}

/* real uid = user, euid = saved uid = 0 (a setuid-root program run by a user) */
static int case_real_vs_effective(void)
{
	if (setresuid(USER, 0, 0) != 0) { puts("FAIL setup setresuid"); return 1; }
	errno = 0;
	CHECK(access(FILE_, R_OK) == -1 && errno == EACCES, "access() used the effective uid");
	errno = 0;
	CHECK(facc2(FILE_, R_OK, 0) == -1 && errno == EACCES, "faccessat2(flags=0) used the effective uid");
	CHECK(facc2(FILE_, R_OK, AT_EACCESS) == 0, "faccessat2(AT_EACCESS) ignored the flag");
	errno = 0;
	/* 3-argument syscall; a stray AT_EACCESS in the 4th register must not count */
	CHECK(syscall(SYS_faccessat, AT_FDCWD, FILE_, R_OK, AT_EACCESS) == -1 && errno == EACCES,
	      "old faccessat read a 4th argument");
	return fails;
}

static int case_flags(void)
{
	errno = 0;
	CHECK(facc2(FILE_, F_OK, 0x1) == -1 && errno == EINVAL, "faccessat2 accepted an unknown flag");
	errno = 0;
	CHECK(facc2(LINK_, F_OK, 0) == -1 && errno == ENOENT, "dangling link followed: must be ENOENT");
	CHECK(facc2(LINK_, F_OK, AT_SYMLINK_NOFOLLOW) == 0, "faccessat2(AT_SYMLINK_NOFOLLOW) followed the link");
	/* AT_EMPTY_PATH: the open file itself */
	int fd = open(FILE_, O_RDONLY);
	CHECK(fd >= 0 && syscall(SYS_faccessat2, fd, "", R_OK | W_OK, AT_EMPTY_PATH) == 0,
	      "faccessat2(fd, \"\", AT_EMPTY_PATH) refused root");
	if (fd >= 0) close(fd);
	errno = 0;
	CHECK(syscall(SYS_faccessat2, 9999, "", R_OK, AT_EMPTY_PATH) == -1 && errno == EBADF,
	      "faccessat2(bad fd, \"\", AT_EMPTY_PATH) not EBADF");
	return fails;
}

static void run(const char *name, int (*fn)(void))
{
	fflush(stdout);
	pid_t p = fork();
	if (p == 0) { int r = fn(); fflush(stdout); _exit(r); }
	int st = 0;
	waitpid(p, &st, 0);
	if (!WIFEXITED(st)) { printf("FAIL %s crashed\n", name); fails++; }
	else fails += WEXITSTATUS(st);
}

int main(void)
{
	if (geteuid() != 0) { puts("FAIL run as root"); return 0; }
	unlink(FILE_);
	unlink(LINK_);
	int fd = open(FILE_, O_CREAT | O_WRONLY, 0600);
	if (fd < 0) { puts("FAIL setup file"); return 0; }
	close(fd);
	chmod(FILE_, 0600);
	if (symlink("/tmp/facc2_nowhere", LINK_) != 0) { puts("FAIL setup symlink"); return 0; }
	run("real-vs-effective", case_real_vs_effective);
	run("flags", case_flags);
	unlink(FILE_);
	unlink(LINK_);
	if (fails) printf("FAIL %d\n", fails); else puts("OK");
	return 0;
}
