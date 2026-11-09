/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* ".." stops at the process root however it is reached: absolute, relative,
 * through a symlink, or from a root that is itself a mount. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int reach(const char *p)
{
	int fd = open(p, O_RDONLY);
	if (fd >= 0) close(fd);
	return fd >= 0;
}

static void touch(const char *p) { close(open(p, O_CREAT | O_WRONLY, 0644)); }

static int in_jail(void)
{
	if (chroot("/tmp/cj/jail") || chdir("/")) return 2;
	if (reach("/../outside")) return 3;
	if (reach("../outside")) return 4;
	if (reach("/sub/../../outside")) return 5;
	if (reach("/../../../../tmp/cj/outside")) return 6;
	if (reach("/sub/up")) return 7;
	if (reach("/sub/absup")) return 8;
	if (chdir("sub") || reach("../../outside")) return 9;
	if (!reach("/inside") || !reach("/../inside") || !reach("../inside")) return 10;
	return 0;
}

static int in_mount(void)
{
	if (chroot("/tmp/cj/mroot") || chdir("/")) return 2;
	if (reach("/../outside")) return 3;
	if (reach("/../cj/outside")) return 4;
	if (!reach("/inm") || !reach("/../inm")) return 5;
	return 0;
}

static int run(int (*fn)(void))
{
	pid_t c = fork();
	if (c == 0) _exit(fn());
	int st = -1;
	waitpid(c, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}

int main(void)
{
	mkdir("/tmp/cj", 0755); mkdir("/tmp/cj/jail", 0755); mkdir("/tmp/cj/jail/sub", 0755);
	mkdir("/tmp/cj/mroot", 0755);
	touch("/tmp/cj/outside"); touch("/tmp/cj/jail/inside");
	symlink("../../outside", "/tmp/cj/jail/sub/up");
	symlink("/../outside", "/tmp/cj/jail/sub/absup");
	if (mount("tmpfs", "/tmp/cj/mroot", "tmpfs", 0, 0)) { puts("FAIL mount"); return 1; }
	touch("/tmp/cj/mroot/inm");

	int a = run(in_jail), b = run(in_mount);
	umount("/tmp/cj/mroot");
	if (a || b) { printf("FAIL jail=%d mount=%d\n", a, b); return 1; }
	puts("OK");
	return 0;
}
