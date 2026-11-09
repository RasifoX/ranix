/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* ".." at the root of a bind mount. A walk that came in through the bind leaves
 * it through the bind's mountpoint; a walk that reached the same directory by
 * its own path goes to its own parent. Found by code reading (2026-11-09): ".."
 * out of any bind's root jumped to that bind's mountpoint for every walk, so
 * after debian-run bound /root under /opt/debian, "/root/.." was /opt/debian,
 * and a bind whose source sits inside a chroot let ".." climb out of it.
 *
 *   test_binddotdot [BASE]     scratch directory, default /tmp/bt (removed first)
 *
 * Layout: BASE/x/a (source, with f, sub, and l -> ../a/f) bound at BASE/y/m;
 * BASE/a bound at BASE/m (same parent); BASE/r bound at BASE/deb/root.
 *   src-dotdot     BASE/x/a/.. is BASE/x
 *   bind-dotdot    BASE/y/m/.. is BASE/y
 *   same-parent    BASE/a/.. and BASE/m/.. are both BASE
 *   sub-dotdot     BASE/y/m/sub/../.. is BASE/y, BASE/x/a/sub/../.. is BASE/x
 *   symlink-src    BASE/x/a/l (-> ../a/f) reads f
 *   getcwd         chdir BASE/x/a/.. then getcwd gives BASE/x
 *   debian-root    BASE/r/.. is BASE; chrooted at BASE/deb, /root/.. is /
 *   chroot-escape  chrooted at BASE/x, /a/.. is / and /a/../m does not exist
 *   chroot-bind    chrooted at BASE/y, /m/.. is /; chrooted at BASE/y/m, /.. is /
 * Prints one PASS/FAIL line a case, then OK or FAIL. Runs as root. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

static char B[200];
static int g_fail;

static void result(const char *name, int ok, const char *why)
{
	printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, ok ? "" : ": ", ok ? "" : why);
	if (!ok) g_fail = 1;
}

static const char *P(const char *rel)
{
	static char buf[4][300];
	static int i;
	i = (i + 1) & 3;
	snprintf(buf[i], sizeof buf[i], "%s%s", B, rel);
	return buf[i];
}

/* 1 if a and b name the same directory */
static int same(const char *a, const char *b)
{
	struct stat sa, sb;
	if (stat(a, &sa) || stat(b, &sb)) return 0;
	return sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

static void put(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) { if (write(fd, text, strlen(text)) < 0) { } close(fd); }
}

static int reads(const char *path, const char *text)
{
	char b[32] = "";
	int fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	ssize_t n = read(fd, b, sizeof b - 1);
	close(fd);
	return n == (ssize_t)strlen(text) && !memcmp(b, text, n);
}

/* run check() in a child chrooted at dir; 1 if it returned 1 */
static int in_chroot(const char *dir, int (*check)(void))
{
	pid_t p = fork();
	if (p == 0) {
		if (chroot(dir) || chdir("/")) _exit(9);
		_exit(check() ? 0 : 1);
	}
	int st = 0;
	waitpid(p, &st, 0);
	return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static int deb_check(void)  { return same("/root/..", "/") && same("/root/../..", "/") && reads("/root/../root/f2", "R"); }
static int esc_check(void)  { return same("/a/..", "/") && access("/a/../m", F_OK) != 0 && same("/a/../a/../..", "/"); }
static int chy_check(void)  { return same("/m/..", "/") && same("/m/../..", "/"); }
static int chm_check(void)  { return same("/..", "/") && same("/sub/../..", "/") && reads("/f", "F"); }

static void unmount_all(void)
{
	umount(P("/y/m"));
	umount(P("/m"));
	umount(P("/deb/root"));
}

int main(int argc, char **argv)
{
	snprintf(B, sizeof B, "%s", argc > 1 ? argv[1] : "/tmp/bt");
	setvbuf(stdout, NULL, _IONBF, 0);
	unmount_all();
	char cmd[260];
	snprintf(cmd, sizeof cmd, "rm -rf %s", B);
	if (system(cmd)) { }
	const char *dirs[] = { "", "/x", "/x/a", "/x/a/sub", "/y", "/y/m", "/a", "/m",
	                       "/r", "/deb", "/deb/root" };
	for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++) mkdir(P(dirs[i]), 0755);
	put(P("/x/a/f"), "F");
	put(P("/r/f2"), "R");
	if (symlink("../a/f", P("/x/a/l"))) { }
	if (mount(P("/x/a"), P("/y/m"), NULL, MS_BIND, NULL) ||
	    mount(P("/a"), P("/m"), NULL, MS_BIND, NULL) ||
	    mount(P("/r"), P("/deb/root"), NULL, MS_BIND, NULL)) {
		printf("FAIL setup: bind mount, errno %d\nFAIL\n", errno);
		unmount_all();
		return 1;
	}
	if (!reads(P("/y/m/f"), "F")) { printf("FAIL setup: bind does not show the source\nFAIL\n"); unmount_all(); return 1; }

	result("src-dotdot", same(P("/x/a/.."), P("/x")), "BASE/x/a/.. is not BASE/x");
	result("bind-dotdot", same(P("/y/m/.."), P("/y")), "BASE/y/m/.. is not BASE/y");
	result("same-parent", same(P("/a/.."), B) && same(P("/m/.."), B), "BASE/a/.. or BASE/m/.. is not BASE");
	result("sub-dotdot", same(P("/y/m/sub/../.."), P("/y")) && same(P("/x/a/sub/../.."), P("/x")),
	       "sub/../.. left the wrong way");
	result("symlink-src", reads(P("/x/a/l"), "F"), "BASE/x/a/l -> ../a/f did not read f");

	char cwd[300] = "";
	int cd = chdir(P("/x/a/..")) == 0 && getcwd(cwd, sizeof cwd) && !strcmp(cwd, P("/x"));
	if (chdir("/")) { }
	char why[400];
	snprintf(why, sizeof why, "getcwd gave '%s'", cwd);
	result("getcwd", cd, why);

	result("debian-root", same(P("/r/.."), B) && in_chroot(P("/deb"), deb_check),
	       "BASE/r/.. is not BASE, or /root/.. in the chroot is not /");
	result("chroot-escape", in_chroot(P("/x"), esc_check), "\"..\" left the chroot through the bind");
	result("chroot-bind", in_chroot(P("/y"), chy_check) && in_chroot(P("/y/m"), chm_check),
	       "\"..\" at a chroot made of a bind went wrong");

	unmount_all();
	if (system(cmd)) { }
	printf("%s\n", g_fail ? "FAIL" : "OK");
	return g_fail;
}
