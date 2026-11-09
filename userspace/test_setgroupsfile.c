/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /proc/self/setgroups (LTP's netns setup for setsockopt05..09): a new user
 * namespace says "allow"; "deny" can be written before the gid_map and then
 * setgroups(2) is EPERM there; "allow" cannot come back, and "deny" cannot be
 * written once the gid_map is. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int bad;

static int put(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return -1;
	int r = write(fd, s, strlen(s)) == (ssize_t)strlen(s) ? 0 : -1;
	close(fd);
	return r;
}

static void want_state(const char *exp)
{
	char b[16] = { 0 };
	int fd = open("/proc/self/setgroups", O_RDONLY);
	if (fd < 0 || read(fd, b, sizeof(b) - 1) <= 0) { printf("read setgroups: errno %d\n", errno); bad++; }
	else if (strcmp(b, exp)) { printf("setgroups says '%s', want '%s'\n", b, exp); bad++; }
	if (fd >= 0) close(fd);
}

static int deny_first(void)
{
	char m[32];
	int uid = getuid(), gid = getgid();
	if (unshare(CLONE_NEWUSER)) { printf("unshare: errno %d\n", errno); return 1; }
	want_state("allow\n");
	if (put("/proc/self/setgroups", "deny")) { printf("write deny: errno %d\n", errno); bad++; }
	want_state("deny\n");
	snprintf(m, sizeof(m), "0 %d 1", uid);
	if (put("/proc/self/uid_map", m)) { printf("uid_map: errno %d\n", errno); bad++; }
	snprintf(m, sizeof(m), "0 %d 1", gid);
	if (put("/proc/self/gid_map", m)) { printf("gid_map: errno %d\n", errno); bad++; }
	errno = 0;
	if (setgroups(0, NULL) != -1 || errno != EPERM) { printf("setgroups after deny: errno %d, want EPERM\n", errno); bad++; }
	errno = 0;
	if (put("/proc/self/setgroups", "allow") != -1 || errno != EPERM) { printf("allow after deny: errno %d, want EPERM\n", errno); bad++; }
	return bad;
}

/* the gid_map written first, by the privileged parent from outside */
static int map_first(int go, int done)
{
	char x;
	if (unshare(CLONE_NEWUSER)) return 1;
	write(go, "u", 1);
	if (read(done, &x, 1) != 1 || x != 'm') { puts("parent could not write the gid_map"); return 1; }
	errno = 0;
	if (put("/proc/self/setgroups", "deny") != -1 || errno != EPERM) { printf("deny after gid_map: errno %d, want EPERM\n", errno); return 1; }
	return 0;
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);   /* the children end with _exit */
	int ok = 1, st;
	pid_t c = fork();
	if (c == 0) _exit(deny_first());
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) ok = 0;

	int up[2], down[2];
	pipe(up); pipe(down);
	c = fork();
	if (c == 0) _exit(map_first(up[1], down[0]));
	char x, path[64], m[32];
	read(up[0], &x, 1);
	snprintf(path, sizeof(path), "/proc/%d/gid_map", c);
	snprintf(m, sizeof(m), "0 %d 1", (int)getgid());
	write(down[1], put(path, m) ? "e" : "m", 1);
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) ok = 0;
	puts(ok ? "OK" : "FAIL");
	return !ok;
}
