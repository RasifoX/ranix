/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The boot /tmp is 01777 as on Linux: any user may create a file in it, and the
 * sticky bit keeps a user from removing or renaming another user's file. Runs
 * before the suite mounts its own tmpfs on /tmp, so it sees the boot directory. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define A "/tmp/.tmpmode_a"
#define B "/tmp/.tmpmode_b"

/* In a child, as uid/gid id: run what and exit with its result (0 = as wanted). */
static int as_user(int id, int what)
{
	pid_t pid = fork();
	if (pid < 0) return 100;
	if (pid == 0) {
		if (setgid(id) || setuid(id)) _exit(101);
		if (what == 1) {                         /* create own file */
			int fd = open(A, O_CREAT | O_EXCL | O_WRONLY, 0644);
			_exit(fd < 0 ? 10 + (errno & 0x3f) : 0);
		}
		if (what == 2) {                         /* may not remove another's */
			if (unlink(A) == 0) _exit(20);
			if (errno != EPERM) _exit(21);
			if (rename(A, B) == 0) _exit(22);
			if (errno != EPERM) _exit(23);
			int fd = open(B, O_CREAT | O_EXCL | O_WRONLY, 0644);   /* but may make its own */
			if (fd < 0) _exit(24);
			close(fd);
			_exit(unlink(B) == 0 ? 0 : 25);     /* and remove its own */
		}
		_exit(102);
	}
	int st = 0;
	if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st)) return 103;
	return WEXITSTATUS(st);
}

int main(void)
{
	struct stat s;
	unlink(A); unlink(B);
	if (stat("/tmp", &s) != 0) { printf("FAIL stat /tmp errno %d\n", errno); return 0; }
	if (!S_ISDIR(s.st_mode) || (s.st_mode & 07777) != 01777) {
		printf("FAIL /tmp mode %o, want 1777\n", (unsigned)(s.st_mode & 07777));
		return 0;
	}
	int r = as_user(1000, 1);
	if (r) { printf("FAIL user 1000 cannot create in /tmp (%d)\n", r); return 0; }
	r = as_user(1001, 2);
	unlink(A); unlink(B);
	if (r) { printf("FAIL user 1001 vs 1000's file (%d)\n", r); return 0; }
	printf("OK\n");
	return 0;
}
