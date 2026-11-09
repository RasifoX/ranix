/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A user may map its own id in a user namespace it made (rootless sandboxes:
 * Chromium, unshare -r), and nothing else: not root's id, not a range, not
 * another task's namespace from outside without the capability. Runs the user
 * part as nobody. Prints OK, or FAIL with what was wrong. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define NOBODY 65534

static int put(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return -errno;
	int r = write(fd, s, strlen(s)) == (ssize_t)strlen(s) ? 0 : -errno;
	close(fd);
	return r;
}

static int get(const char *path, char *buf, int cap)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	int n = read(fd, buf, cap - 1);
	close(fd);
	buf[n > 0 ? n : 0] = 0;
	return n;
}

/* as nobody, in a fresh user namespace: the refusals, then its own id */
static int own_ns(void)
{
	char b[64];
	if (unshare(CLONE_NEWUSER)) { printf("FAIL unshare %d\n", errno); return 1; }
	if (!put("/proc/self/uid_map", "0 0 1\n")) { puts("FAIL nobody mapped root's uid"); return 1; }
	if (!put("/proc/self/uid_map", "0 65534 2\n")) { puts("FAIL nobody mapped a range"); return 1; }
	if (!put("/proc/self/uid_map", "0 65534 1\n1 0 1\n")) { puts("FAIL nobody wrote two lines"); return 1; }
	if (!put("/proc/self/gid_map", "0 0 1\n")) { puts("FAIL nobody mapped root's gid"); return 1; }
	int r = put("/proc/self/uid_map", "0 65534 1\n");
	if (r) { printf("FAIL own uid map refused %d\n", r); return 1; }
	r = put("/proc/self/gid_map", "0 65534 1\n");
	if (r) { printf("FAIL own gid map refused %d\n", r); return 1; }
	get("/proc/self/uid_map", b, sizeof b);
	if (strncmp(b, "0 65534 1", 9)) { printf("FAIL uid_map reads '%s'\n", b); return 1; }
	if (getuid() != 0) { printf("FAIL uid inside %d, want 0\n", (int)getuid()); return 1; }
	return 0;
}

int main(void)
{
	if (getuid() != 0) { puts("FAIL run as root"); return 0; }
	/* 1. nobody's own namespace */
	pid_t c = fork();
	if (c == 0) {
		if (setgid(NOBODY) || setuid(NOBODY)) _exit(2);
		_exit(own_ns());
	}
	int st = 0;
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) { fflush(stdout); if (WEXITSTATUS(st) == 2) puts("FAIL setuid"); return 0; }

	/* 2. from outside: a namespace made by root's child; nobody (no capability)
	 *    may not map it, and may not even open it for writing */
	int go[2], done[2];
	if (pipe(go) || pipe(done)) { puts("FAIL pipe"); return 0; }
	pid_t t = fork();
	if (t == 0) {
		char x;
		if (unshare(CLONE_NEWUSER)) _exit(1);
		if (write(done[1], "r", 1) != 1) _exit(1);
		if (read(go[0], &x, 1) < 0) _exit(1);
		_exit(0);
	}
	char x, p[64];
	if (read(done[0], &x, 1) != 1) { puts("FAIL target"); return 0; }
	snprintf(p, sizeof p, "/proc/%d/uid_map", (int)t);
	pid_t o = fork();
	if (o == 0) {
		if (setgid(NOBODY) || setuid(NOBODY)) _exit(2);
		_exit(put(p, "0 65534 1\n") == 0 ? 1 : 0);
	}
	waitpid(o, &st, 0);
	int outsider_ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
	/* root of the machine may map it */
	int root_ok = put(p, "0 0 1\n") == 0;
	if (write(go[1], "g", 1) != 1) { puts("FAIL go"); }
	waitpid(t, NULL, 0);
	if (!outsider_ok) { puts("FAIL nobody wrote another task's uid_map"); return 0; }
	if (!root_ok) { puts("FAIL root could not map a child namespace"); return 0; }
	puts("OK");
	return 0;
}
