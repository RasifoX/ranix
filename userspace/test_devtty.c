/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A new session has no controlling terminal, so /dev/tty does not open
 * (ENXIO). "bash -i" with stderr on a pipe opens /dev/tty to find its
 * terminal; given the console it looped waiting to be its foreground (VS Code
 * could not read the login shell's environment). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void)
{
	pid_t c = fork();
	if (c == 0) {
		if (setsid() < 0) _exit(2);
		int fd = open("/dev/tty", O_RDWR | O_NONBLOCK);
		_exit(fd < 0 && errno == ENXIO ? 0 : 3);
	}
	int st;
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("FAIL status %d\n", st); return 0; }
	printf("OK\n");
	return 0;
}
