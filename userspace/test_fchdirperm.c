/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* fchdir() needs search permission on the directory, as chdir() does. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
	const char *d = "/tmp/fchdirperm";
	mkdir(d, 0700);
	if (chown(d, 1000, 1000) != 0 || chmod(d, 0400) != 0) { printf("FAIL setup errno=%d\n", errno); return 1; }
	pid_t p = fork();
	if (p == 0) {
		if (setgid(1000) || setuid(1000)) _exit(10);
		int fd = open(d, O_RDONLY | O_DIRECTORY);
		if (fd < 0) _exit(11);
		errno = 0;
		int c = chdir(d), ce = errno;
		errno = 0;
		int f = fchdir(fd), fe = errno;
		if (c != -1 || ce != EACCES) _exit(12);      /* control */
		if (f != -1 || fe != EACCES) _exit(13);
		/* with search permission it works */
		if (fchmod(fd, 0500) != 0) _exit(14);
		if (fchdir(fd) != 0) _exit(15);
		_exit(0);
	}
	int st = 0;
	waitpid(p, &st, 0);
	rmdir(d);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { printf("FAIL step=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1); return 1; }
	puts("OK");
	return 0;
}
