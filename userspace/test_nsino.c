/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /proc/<pid>/ns/<kind> identity: stat() of the link gives the namespace's
 * own number as st_ino, as Linux nsfs does, so two processes sharing a
 * namespace agree and the number matches readlink's "mnt:[N]". gdb compares
 * these (linux_ns_same) before it attaches; a mismatch sent it down a
 * setns helper path that does not work. After unshare the number changes. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/wait.h>

static unsigned long link_num(const char *path)
{
	char b[64];
	ssize_t n = readlink(path, b, sizeof b - 1);
	if (n <= 0) return 0;
	b[n] = 0;
	char *o = strchr(b, '[');
	return o ? strtoul(o + 1, NULL, 10) : 0;
}

int main(void)
{
	struct stat a, b;
	char p[64];
	snprintf(p, sizeof p, "/proc/%d/ns/mnt", (int)getppid());
	if (stat("/proc/self/ns/mnt", &a) != 0 || stat(p, &b) != 0) { printf("FAIL stat\n"); return 0; }
	if (a.st_ino != b.st_ino || a.st_dev != b.st_dev) { printf("FAIL differ %lu %lu\n", (unsigned long)a.st_ino, (unsigned long)b.st_ino); return 0; }
	if (a.st_ino != link_num("/proc/self/ns/mnt")) { printf("FAIL not-link-number\n"); return 0; }
	struct stat u1;
	if (stat("/proc/self/ns/uts", &u1) != 0 || u1.st_ino != link_num("/proc/self/ns/uts")) { printf("FAIL uts\n"); return 0; }
	pid_t c = fork();
	if (c == 0) {
		struct stat u2;
		if (unshare(CLONE_NEWUTS) != 0) _exit(0);    /* no permission: nothing to compare */
		if (stat("/proc/self/ns/uts", &u2) != 0) _exit(2);
		_exit(u2.st_ino != u1.st_ino ? 0 : 3);
	}
	int st;
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("FAIL unshare %d\n", st); return 0; }
	printf("OK\n");
	return 0;
}
