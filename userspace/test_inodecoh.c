/* Inode coherency on a disk filesystem: a change to a file's or directory's
 * attributes must hold for every later lookup, whichever in-memory copy of the
 * inode the path walk finds. Found on Casper (2026-11-08): on ext2, chmod 700 of
 * a directory that was already looked up left it open to other users.
 *
 *   test_inodecoh [DIR]     DIR on the disk filesystem to test (default /mnt)
 *
 * Each case warms the caches first (stat, open, exec as root and as the other
 * user), then changes the attribute, then checks as the other user:
 *   chmod-narrow   dir 0755 -> 0700: nobody must get EACCES on DIR/d/f
 *   chmod-fresh    the same, but chmod right after mkdir + create, nothing warmed
 *   chmod-at       fchmodat(parentfd, name)
 *   chmod-nofollow fchmodat(AT_SYMLINK_NOFOLLOW); chmod-procfd: chmod("/proc/self/fd/N")
 *                  of an O_PATH fd, which is how musl does the former
 *   chmod-widen    dir 0700 -> 0755: nobody must now read DIR/d/f
 *   chown-narrow   file 0600 owned by nobody -> chown root: nobody must get EACCES
 *   symlink-retarget  l -> a, read; then l -> b: a read through l must give b
 *   chmod-reg4     fchmodat (three arguments) with AT_SYMLINK_NOFOLLOW left in the
 *                  fourth register, as musl's chmod can: it must chmod, and hold
 *   fchmodat2-link fchmodat2(AT_SYMLINK_NOFOLLOW) on a symlink: EOPNOTSUPP, target kept
 *   lchown-dir     lchown of a dir nobody owns to root: nobody must get EACCES
 *   hardlink-chmod chmod 600 through one name: nobody must get EACCES through the other
 * Prints one PASS/FAIL line a case, then OK or FAIL. Runs as root. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define NOBODY 65534

static char g_dir[256];
static int g_fail;

static void put(const char *path, const char *text, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd >= 0) { if (write(fd, text, strlen(text)) < 0) { } close(fd); }
	chmod(path, mode);
}

/* read PATH as nobody: 0 = read EXPECT, 1 = EACCES, 2 = other error, 3 = other text */
static int read_as_nobody(const char *path, const char *expect)
{
	pid_t p = fork();
	if (p == 0) {
		if (setgroups(0, NULL) || setgid(NOBODY) || setuid(NOBODY)) _exit(9);
		int fd = open(path, O_RDONLY);
		if (fd < 0) _exit(errno == EACCES ? 1 : 2);
		char b[64] = "";
		ssize_t n = read(fd, b, sizeof b - 1);
		close(fd);
		if (n < 0) _exit(errno == EACCES ? 1 : 2);
		b[n] = 0;
		_exit(expect && strcmp(b, expect) ? 3 : 0);
	}
	int st;
	if (p < 0 || waitpid(p, &st, 0) != p || !WIFEXITED(st)) return 2;
	return WEXITSTATUS(st);
}

static void result(const char *name, int ok, const char *why)
{
	printf("%s: %s%s%s\n", ok ? "PASS" : "FAIL", name, ok ? "" : " - ", ok ? "" : why);
	if (!ok) g_fail = 1;
}

static void warm(const char *path)
{
	struct stat st;
	stat(path, &st);
	int fd = open(path, O_RDONLY);
	if (fd >= 0) close(fd);
}

int main(int argc, char **argv)
{
	if (geteuid() != 0) { puts("FAIL: run as root"); return 0; }
	snprintf(g_dir, sizeof g_dir, "%s/inodecoh.%d", argc > 1 ? argv[1] : "/mnt", (int)getpid());
	if (mkdir(g_dir, 0755) != 0) { printf("FAIL: mkdir %s: %s\n", g_dir, strerror(errno)); return 0; }
	char d[300], f[320], l[320], a[320], b[320];

	/* chmod-narrow */
	snprintf(d, sizeof d, "%s/n", g_dir); snprintf(f, sizeof f, "%s/f", d);
	mkdir(d, 0755); chmod(d, 0755); put(f, "secret", 0644);
	warm(d); warm(f);
	int r = read_as_nobody(f, "secret");                 /* the other user caches it too */
	if (r != 0) result("chmod-narrow", 0, "setup: nobody could not read before chmod");
	else {
		chmod(d, 0700);
		r = read_as_nobody(f, NULL);
		result("chmod-narrow", r == 1, r == 0 ? "nobody still reads through a 0700 directory" : "unexpected error");
	}

	/* chmod-fresh: as on Casper, no lookup by anyone between create and chmod */
	snprintf(d, sizeof d, "%s/c", g_dir); snprintf(f, sizeof f, "%s/f", d);
	mkdir(d, 0755);
	put(f, "secret", 0644);
	chmod(d, 0700);
	r = read_as_nobody(f, NULL);
	result("chmod-fresh", r == 1, r == 0 ? "nobody reads through a just-made dir chmod 700" : "unexpected error");

	/* chmod-at: as GNU chmod does it, fchmodat relative to an open parent dir
	 * (found: busybox chmod works, coreutils chmod leaves the dir open) */
	snprintf(d, sizeof d, "%s/t", g_dir); snprintf(f, sizeof f, "%s/f", d);
	mkdir(d, 0755);
	put(f, "secret", 0644);
	int pfd = open(g_dir, O_RDONLY | O_DIRECTORY);
	int at = pfd >= 0 ? fchmodat(pfd, "t", 0700, 0) : -1;
	if (pfd >= 0) close(pfd);
	r = read_as_nobody(f, NULL);
	result("chmod-at", at == 0 && r == 1, at ? "fchmodat failed" : r == 0 ? "nobody reads through a dir fchmodat 700" : "unexpected error");

	/* chmod-nofollow: fchmodat(..., AT_SYMLINK_NOFOLLOW), which musl does as
	 * open(O_PATH) + chmod("/proc/self/fd/N"); and that path directly */
	snprintf(d, sizeof d, "%s/p", g_dir); snprintf(f, sizeof f, "%s/f", d);
	mkdir(d, 0755);
	put(f, "secret", 0644);
	at = fchmodat(AT_FDCWD, d, 0700, AT_SYMLINK_NOFOLLOW);
	r = read_as_nobody(f, NULL);
	result("chmod-nofollow", at == 0 && r == 1, at ? "fchmodat NOFOLLOW failed" : r == 0 ? "nobody reads through a dir fchmodat NOFOLLOW 700" : "unexpected error");
	if (at) printf("     (fchmodat NOFOLLOW errno %d)\n", errno);
	/* the fchmodat2 system call itself (452), as a newer libc calls it */
	snprintf(d, sizeof d, "%s/s", g_dir); snprintf(f, sizeof f, "%s/f", d);
	mkdir(d, 0755);
	put(f, "secret", 0644);
	long s2 = syscall(452, AT_FDCWD, d, 0700, AT_SYMLINK_NOFOLLOW);
	int e2 = errno;
	r = read_as_nobody(f, NULL);
	if (s2 != 0) printf("     (fchmodat2 rc %ld errno %d)\n", s2, e2);
	result("chmod-fchmodat2", s2 == 0 && r == 1, s2 ? "fchmodat2 NOFOLLOW refused a directory" : "nobody still reads through the dir");
	snprintf(d, sizeof d, "%s/q", g_dir); snprintf(f, sizeof f, "%s/f", d);
	mkdir(d, 0755);
	put(f, "secret", 0644);
	int qfd = open(d, O_PATH | O_DIRECTORY);
	char pp[64];
	snprintf(pp, sizeof pp, "/proc/self/fd/%d", qfd);
	at = qfd >= 0 ? chmod(pp, 0700) : -1;
	if (qfd >= 0) close(qfd);
	r = read_as_nobody(f, NULL);
	result("chmod-procfd", at == 0 && r == 1, at ? "chmod /proc/self/fd/N failed" : r == 0 ? "nobody reads through a dir chmod'ed via /proc/self/fd" : "unexpected error");

	/* chmod-widen */
	snprintf(d, sizeof d, "%s/w", g_dir); snprintf(f, sizeof f, "%s/f", d);
	mkdir(d, 0700); chmod(d, 0700); put(f, "open", 0644);
	warm(d); warm(f);
	r = read_as_nobody(f, NULL);
	if (r != 1) result("chmod-widen", 0, "setup: nobody read through 0700 before chmod");
	else {
		chmod(d, 0755);
		r = read_as_nobody(f, "open");
		result("chmod-widen", r == 0, "nobody still refused after chmod 755");
	}

	/* chown-narrow */
	snprintf(f, sizeof f, "%s/owned", g_dir);
	put(f, "mine", 0600);
	if (chown(f, NOBODY, NOBODY) != 0) { }
	warm(f);
	r = read_as_nobody(f, "mine");
	if (r != 0) result("chown-narrow", 0, "setup: nobody could not read its own file");
	else {
		if (chown(f, 0, 0) != 0) { }
		r = read_as_nobody(f, NULL);
		result("chown-narrow", r == 1, r == 0 ? "nobody still reads a file chowned to root" : "unexpected error");
	}

	/* symlink-retarget */
	snprintf(a, sizeof a, "%s/a", g_dir); snprintf(b, sizeof b, "%s/b", g_dir); snprintf(l, sizeof l, "%s/l", g_dir);
	put(a, "AAA", 0644); put(b, "BBB", 0644);
	if (symlink("a", l) != 0) { }
	warm(l);
	r = read_as_nobody(l, "AAA");
	if (r != 0) result("symlink-retarget", 0, "setup: read through l -> a failed");
	else {
		unlink(l);
		if (symlink("b", l) != 0) { }
		r = read_as_nobody(l, "BBB");
		result("symlink-retarget", r == 0, r == 3 ? "read through l still gives a's content" : "read through new l failed");
	}

	/* chmod-reg4: the old fchmodat has no flags; a stray fourth register must not
	 * turn it into lchmod, which once wrote a copy no path walk saw */
	snprintf(d, sizeof d, "%s/r4", g_dir); snprintf(f, sizeof f, "%s/f", d);
	mkdir(d, 0755); chmod(d, 0755); put(f, "secret", 0644);
	warm(d); warm(f);
	r = read_as_nobody(f, "secret");
	if (r != 0) result("chmod-reg4", 0, "setup: nobody could not read before chmod");
	else {
		long c4 = syscall(SYS_fchmodat, AT_FDCWD, d, 0700, AT_SYMLINK_NOFOLLOW);
		struct stat st;
		int m = stat(d, &st) == 0 ? (int)(st.st_mode & 07777) : -1;
		r = read_as_nobody(f, NULL);
		result("chmod-reg4", c4 == 0 && m == 0700 && r == 1,
		       c4 ? "fchmodat failed" : m != 0700 ? "mode not 0700" : "nobody still reads through the dir");
	}

	/* fchmodat2-link: a symlink has no mode; the target must not change */
	snprintf(a, sizeof a, "%s/t2", g_dir); snprintf(l, sizeof l, "%s/t2l", g_dir);
	put(a, "x", 0644);
	if (symlink("t2", l) != 0) { }
	long s3 = syscall(452, AT_FDCWD, l, 0600, AT_SYMLINK_NOFOLLOW);
	int e3 = errno;
	struct stat ts;
	int tm = stat(a, &ts) == 0 ? (int)(ts.st_mode & 07777) : -1;
	result("fchmodat2-link", s3 == -1 && e3 == EOPNOTSUPP && tm == 0644,
	       s3 == 0 ? "chmod of a symlink said yes" : tm != 0644 ? "the target's mode changed" : "wrong errno");

	/* lchown-dir: lchown on a directory acts on it, for every later walk */
	snprintf(d, sizeof d, "%s/o", g_dir); snprintf(f, sizeof f, "%s/f", d);
	mkdir(d, 0700); chmod(d, 0700); put(f, "secret", 0644);
	if (chown(d, NOBODY, NOBODY) != 0) { }
	warm(d); warm(f);
	r = read_as_nobody(f, "secret");
	if (r != 0) result("lchown-dir", 0, "setup: nobody could not read inside its own dir");
	else {
		long lc = syscall(SYS_lchown, d, 0, 0);
		r = read_as_nobody(f, NULL);
		result("lchown-dir", lc == 0 && r == 1, lc ? "lchown failed" : r == 0 ? "nobody still enters a dir lchowned to root" : "unexpected error");
	}

	/* hardlink-chmod: two names, one file: a chmod through a holds through b */
	snprintf(a, sizeof a, "%s/h1", g_dir); snprintf(b, sizeof b, "%s/h2", g_dir);
	put(a, "secret", 0644);
	if (link(a, b) != 0) { }
	warm(a); warm(b);
	r = read_as_nobody(b, "secret");
	if (r != 0) result("hardlink-chmod", 0, "setup: nobody could not read the second name");
	else {
		chmod(a, 0600);
		r = read_as_nobody(b, NULL);
		result("hardlink-chmod", r == 1, r == 0 ? "nobody still reads through the other name" : "unexpected error");
	}

	/* clean up as root */
	char cmd[400];
	snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir);
	if (system(cmd) != 0) { }
	puts(g_fail ? "FAIL" : "OK");
	return 0;
}
