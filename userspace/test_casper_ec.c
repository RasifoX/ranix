/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /sys/kernel/ranix_ec, the Casper EC "OS present" door: 0600, root reads its
 * state, an unprivileged user can neither read nor write it, a bad command is
 * refused, and on any other machine (QEMU) "on" is refused with ENODEV. Never
 * writes "on" where the board matches: that is for a person to decide. Prints OK. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define NODE "/sys/kernel/ranix_ec"

static int write_cmd(const char *s)
{
	int fd = open(NODE, O_WRONLY);
	if (fd < 0) return -errno;
	int r = (int)write(fd, s, strlen(s));
	int e = errno;
	close(fd);
	return r < 0 ? -e : 0;
}

/* as uid 65534: 0 when both read and write are denied */
static int unprivileged(void)
{
	pid_t p = fork();
	if (p == 0) {
		if (setgid(65534) || setuid(65534)) _exit(2);
		char b[64];
		int fd = open(NODE, O_RDONLY);
		if (fd >= 0) {
			if (read(fd, b, sizeof(b)) >= 0 || errno != EACCES) _exit(3);
			close(fd);
		}
		fd = open(NODE, O_WRONLY);
		if (fd >= 0) {
			if (write(fd, "on", 2) >= 0 || errno != EPERM) _exit(4);
			close(fd);
		}
		_exit(0);
	}
	int st;
	if (p < 0 || waitpid(p, &st, 0) != p || !WIFEXITED(st)) return -1;
	return WEXITSTATUS(st);
}

int main(void)
{
	struct stat st;
	if (stat(NODE, &st) || !S_ISREG(st.st_mode) || (st.st_mode & 0777) != 0600) { puts("FAIL mode"); return 1; }
	char b[512] = {0};
	int fd = open(NODE, O_RDONLY);
	if (fd < 0) { puts("FAIL open"); return 1; }
	ssize_t n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0 || b[n - 1] != '\n' || strncmp(b, "board ", 6) || !strstr(b, "\nosflags ")) { puts("FAIL format"); return 1; }
	int r = unprivileged();
	if (r) { printf("FAIL unprivileged %d\n", r); return 1; }
	if (write_cmd("bogus") != -EINVAL) { puts("FAIL bad command"); return 1; }
	if (!strncmp(b, "board none", 10)) {
		if (write_cmd("on") != -ENODEV || write_cmd("off") != -ENODEV) { puts("FAIL other board"); return 1; }
		fd = open(NODE, O_RDONLY);
		n = fd >= 0 ? read(fd, b, sizeof(b) - 1) : -1;
		if (fd >= 0) close(fd);
		if (n <= 0 || !strstr(b, "osflags off\n")) { puts("FAIL still off"); return 1; }
	}
	puts("OK");
	return 0;
}
