/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /dev/stdin, /dev/stdout and /dev/stderr reach the process's fd 0, 1 and 2.
 * What may be done through them is what that fd was opened for, not what the
 * new open of the node asks: a file root handed over read-only is not written
 * through /dev/stdin, and a write-only (append) log is not read back through
 * /dev/stdout. Each case runs in a child that is not root.
 * Prints FAIL lines, then OK or FAIL. Runs as root. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int as_user_with(int fd, int as, int (*fn)(void))
{
	pid_t c = fork();
	if (c == 0) {
		if (dup2(fd, as) != as) _exit(9);
		if (setgid(65534) || setuid(65534)) _exit(9);
		_exit(fn());
	}
	int st = 0;
	waitpid(c, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 100;
}

/* the child: write through /dev/stdin; 0 when nothing got written */
static int write_stdin(void)
{
	int w = open("/dev/stdin", O_WRONLY);
	if (w < 0) return 0;
	return write(w, "user", 4) > 0 ? 1 : 0;
}

/* the child: read through /dev/stdout; 0 when nothing came back */
static int read_stdout(void)
{
	char b[16];
	lseek(1, 0, SEEK_SET);
	int r = open("/dev/stdout", O_RDONLY);
	if (r < 0) return 0;
	return read(r, b, sizeof b) > 0 ? 1 : 0;
}

int main(void)
{
	int fd = open("/tmp/devfd-ro", O_RDWR | O_CREAT | O_TRUNC, 0644);
	CHECK(fd >= 0 && write(fd, "root", 4) == 4, "make /tmp/devfd-ro");
	close(fd);
	fd = open("/tmp/devfd-ro", O_RDONLY);
	CHECK(as_user_with(fd, 0, write_stdin) == 0, "a read-only fd 0 was written through /dev/stdin");
	char back[8] = "";
	CHECK(pread(fd, back, 4, 0) == 4 && !memcmp(back, "root", 4), "the root file changed through /dev/stdin");
	close(fd);
	unlink("/tmp/devfd-ro");

	fd = open("/tmp/devfd-log", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0640);
	CHECK(fd >= 0 && write(fd, "secret", 6) == 6, "make /tmp/devfd-log");
	CHECK(as_user_with(fd, 1, read_stdout) == 0, "a write-only fd 1 (root's 0640 log) was read through /dev/stdout");
	close(fd);
	unlink("/tmp/devfd-log");
	puts(fails ? "FAIL" : "OK");
	return fails != 0;
}
