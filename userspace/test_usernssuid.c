/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* exec of a setuid/setgid file inside a user namespace: the file's owner must
 * have an id in that namespace, or the bits do nothing (Linux bprm_fill_uid).
 * Otherwise a user who makes a namespace and runs a setuid-root binary there
 * (hardlinked into a directory of their own) is root of the machine.
 *   A. control: nobody runs the setuid-root copy outside any namespace; it
 *      must become root (else the test proves nothing)
 *   B. nobody, in a user namespace of its own (root unmapped), runs it through
 *      its own hardlink: the euid must stay nobody's, root's file unreadable
 *   C. a namespace where root is mapped (made by root): the bits still work
 * Prints OK, or FAIL with what was wrong. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define NOBODY 65534
#define DIR_   "/tmp/L_us"
#define BIN    DIR_ "/h"
#define LINK   DIR_ "/nb/h"
#define SECRET DIR_ "/secret"

static int put(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return -errno;
	int r = write(fd, s, strlen(s)) == (ssize_t)strlen(s) ? 0 : -errno;
	close(fd);
	return r;
}

static int copy_file(const char *src, const char *dst)
{
	int in = open(src, O_RDONLY);
	if (in < 0) return -1;
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (out < 0) { close(in); return -1; }
	char buf[4096]; ssize_t n;
	while ((n = read(in, buf, sizeof buf)) > 0)
		if (write(out, buf, (size_t)n) != n) { n = -1; break; }
	close(in); close(out);
	return n < 0 ? -1 : 0;
}

/* the setuid image: what ids did exec give, and can it read root's file? */
static int probe(void)
{
	int fd = open(SECRET, O_RDONLY);
	printf("%d %d %d\n", (int)geteuid(), (int)getegid(), fd >= 0);
	return 0;
}

static int become_nobody(void)
{
	return setgroups(0, NULL) || setgid(NOBODY) || setuid(NOBODY);
}

/* map one id line in the caller's own new namespace */
static int map_self(const char *uidline, const char *gidline)
{
	if (unshare(CLONE_NEWUSER)) return 1;
	if (put("/proc/self/uid_map", uidline)) return 2;
	if (put("/proc/self/gid_map", gidline)) return 3;
	return 0;
}

/* run case `which` in a child; its probe line comes back in out. For C the
 * parent (root of the machine) writes the child's maps from outside, as
 * newuidmap does: a two-line map needs CAP_SETUID in the parent namespace. */
static int run_case(int which, const char *path, char *out, int cap)
{
	int pfd[2], ready[2], go[2];
	if (pipe(pfd) || pipe(ready) || pipe(go)) return -1;
	pid_t c = fork();
	if (c == 0) {
		char x;
		dup2(pfd[1], 1); close(pfd[0]); close(pfd[1]);
		if (which == 'A' && become_nobody()) _exit(10);
		if (which == 'B' && (become_nobody() || map_self("0 65534 1\n", "0 65534 1\n"))) _exit(11);
		if (which == 'C') {
			if (unshare(CLONE_NEWUSER)) _exit(12);
			if (write(ready[1], "r", 1) != 1 || read(go[0], &x, 1) != 1) _exit(12);
			if (setgid(NOBODY) || setuid(NOBODY)) _exit(13);   /* nobody inside */
		}
		execl(path, "h", "probe", (char *)0);
		_exit(127);
	}
	close(pfd[1]);
	if (which == 'C') {
		char x, p[64];
		if (read(ready[0], &x, 1) == 1) {
			snprintf(p, sizeof p, "/proc/%d/uid_map", (int)c);
			int e1 = put(p, "0 0 1\n65534 65534 1\n");
			snprintf(p, sizeof p, "/proc/%d/gid_map", (int)c);
			int e2 = put(p, "0 0 1\n65534 65534 1\n");
			if (e1 || e2) printf("  C map %d %d\n", e1, e2);
		}
		if (write(go[1], "g", 1) != 1) { }
	}
	int n = (int)read(pfd[0], out, cap - 1);
	close(pfd[0]);
	close(ready[0]); close(ready[1]); close(go[0]); close(go[1]);
	int st = 0;
	waitpid(c, &st, 0);
	out[n > 0 ? n : 0] = 0;
	char *nl = strchr(out, '\n'); if (nl) *nl = 0;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "probe")) return probe();
	if (getuid() != 0) { puts("FAIL run as root"); return 0; }

	mkdir(DIR_, 0755);
	if (mount("tmpfs", DIR_, "tmpfs", 0, NULL)) { printf("FAIL mount %d\n", errno); return 0; }
	chmod(DIR_, 0755);
	char self[256];
	ssize_t sl = readlink("/proc/self/exe", self, sizeof self - 1);
	self[sl > 0 ? sl : 0] = 0;
	if ((!self[0] || copy_file(self, BIN)) && copy_file("/bin/test_usernssuid", BIN)) {
		printf("FAIL copy %d\n", errno); return 0;
	}
	if (chown(BIN, 0, 0) || chmod(BIN, 06755)) { printf("FAIL chmod %d\n", errno); return 0; }
	int sfd = open(SECRET, O_WRONLY | O_CREAT, 0600);
	if (sfd < 0) { printf("FAIL secret %d\n", errno); return 0; }
	close(sfd);
	mkdir(DIR_ "/nb", 0755);
	if (chown(DIR_ "/nb", NOBODY, NOBODY)) { }

	/* nobody makes its own hardlink to the setuid file, where the kernel allows it */
	const char *bpath = BIN;
	pid_t l = fork();
	if (l == 0) { if (become_nobody()) _exit(1); _exit(link(BIN, LINK) ? 1 : 0); }
	int st = 0;
	waitpid(l, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0) bpath = LINK;
	printf("  B runs %s\n", bpath);

	char a[64], b[64], c[64];
	int ra = run_case('A', BIN, a, sizeof a);
	int rb = run_case('B', bpath, b, sizeof b);
	int rc = run_case('C', BIN, c, sizeof c);
	printf("  A rc=%d '%s'  B rc=%d '%s'  C rc=%d '%s'\n", ra, a, rb, b, rc, c);

	int fail = 0;
	/* A: outside, euid 0 and egid 0, the secret opens */
	if (ra || strcmp(a, "0 0 1")) { puts("FAIL control: setuid-root did not work outside a namespace"); fail = 1; }
	/* B: inside, nobody is uid 0 of its namespace; root (unmapped) would read 65534 */
	else if (rb) { printf("FAIL B setup rc=%d (unshare or own map)\n", rb); fail = 1; }
	else if (strcmp(b, "0 0 0")) { puts("FAIL setuid-root raised a user namespace task to an unmapped root"); fail = 1; }
	/* C: root mapped as 0 inside: the bits apply */
	else if (rc || strcmp(c, "0 0 1")) { puts("FAIL setuid did not apply where root is mapped"); fail = 1; }

	unlink(LINK); unlink(BIN); unlink(SECRET);
	umount(DIR_);
	if (!fail) puts("OK");
	return 0;
}
