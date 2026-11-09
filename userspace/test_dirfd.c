/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A lookup relative to a directory fd starts at that directory itself (Linux):
 * not at whatever its path names now. So it still works after the caller
 * chroots away from it (Chromium's sandbox reads /proc/self/fd through a /proc
 * fd opened before its chroot), and after the directory is renamed or removed.
 * The negative half: a chrooted task that holds no such fd cannot get out, by
 * "..", by absolute or relative symlinks, or by a dirfd opened inside; and one
 * that does hold one cannot mix it with a path from its own root (EXDEV).
 * /proc/self/fd/N names the open file itself (stat of a pipe, a socket, a file
 * opened before the chroot), and another user's fd/N is not there.
 * Runs as root. Prints OK, or FAIL with the first thing that was wrong. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <sched.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char B[64];          /* the test's base directory */
static int  msgfd = -1;     /* child -> parent: why it failed */

static int fail(const char *what)
{
	char m[256];
	int n = snprintf(m, sizeof m, "%s (errno %d)", what, errno);
	if (write(msgfd, m, (size_t)n) < 0) { }
	return 1;
}

static void put(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) { if (write(fd, s, strlen(s)) < 0) { } close(fd); }
}

/* "" if it cannot be opened or read */
static void get_at(int dfd, const char *name, char *out, int cap)
{
	out[0] = 0;
	int fd = openat(dfd, name, O_RDONLY);
	if (fd < 0) return;
	int n = (int)read(fd, out, (size_t)cap - 1);
	close(fd);
	out[n > 0 ? n : 0] = 0;
}

static int same(const struct stat *a, const struct stat *b)
{
	return a->st_ino == b->st_ino && a->st_dev == b->st_dev;
}

static int jail(void)
{
	char j[96];
	snprintf(j, sizeof j, "%s/jail", B);
	return chroot(j) || chdir("/");
}

/* (a) /proc opened before chroot into a plain directory */
static int t_proc_jail(void)
{
	char l[32], want[32];
	ssize_t ln = readlink("/proc/self", l, sizeof l - 1);
	l[ln > 0 ? ln : 0] = 0;
	snprintf(want, sizeof want, "%d", (int)getpid());
	if (strcmp(l, want)) return fail("/proc/self does not read as the bare pid (Linux)");
	int pfd = open("/proc", O_RDONLY | O_DIRECTORY);
	if (pfd < 0) return fail("open /proc");
	if (jail()) return fail("chroot jail");
	int fd = openat(pfd, "self/fd/", O_RDONLY | O_DIRECTORY);
	if (fd < 0) return fail("openat(proc_fd, self/fd/) after chroot");
	struct stat st;
	if (fstatat(fd, "0", &st, 0)) return fail("fstatat(self_fd, 0) after chroot");
	char s[64];
	get_at(pfd, "self/status", s, sizeof s);
	if (strncmp(s, "Name:", 5)) return fail("self/status through proc_fd");
	return 0;
}

/* (a) the same with Chromium's empty root: chroot into a procfs directory */
static int t_proc_empty(void)
{
	int pfd = open("/proc", O_RDONLY | O_DIRECTORY);
	if (pfd < 0) return fail("open /proc");
	if (chroot("/proc/self/fdinfo/") || chdir("/")) return fail("chroot fdinfo");
	if (open("/proc", O_RDONLY) >= 0) return fail("/proc visible in the empty root");
	int fd = openat(pfd, "self/fd/", O_RDONLY | O_DIRECTORY);
	if (fd < 0) return fail("openat(proc_fd, self/fd/) in the empty root");
	/* ProcUtil::HasOpenDirectory: each entry by fstatat on that fd */
	struct stat st;
	char n[16];
	snprintf(n, sizeof n, "%d", pfd);
	if (fstatat(fd, n, &st, 0) || !S_ISDIR(st.st_mode)) return fail("fstatat(self_fd, proc_fd) in the empty root");
	if (fstatat(fd, "0", &st, 0) || S_ISDIR(st.st_mode)) return fail("fstatat(self_fd, 0) in the empty root");
	return 0;
}

/* (a) a held directory of the base: its files, and nothing more for others */
static int t_held_base(void)
{
	int bfd = open(B, O_RDONLY | O_DIRECTORY);
	if (bfd < 0) return fail("open base");
	if (jail()) return fail("chroot jail");
	char s[16];
	get_at(bfd, "secret", s, sizeof s);
	if (strcmp(s, "s")) return fail("openat(held fd, secret) after chroot");
	/* mixing the held fd with the jail's own root in one call */
	if (renameat(bfd, "secret", AT_FDCWD, "/stolen") == 0) return fail("mixed renameat succeeded");
	if (errno != EXDEV) return fail("mixed renameat: not EXDEV");
	if (renameat(AT_FDCWD, "/in", bfd, "stolen") == 0 || errno != EXDEV)
		return fail("mixed renameat (other order): not EXDEV");
	return 0;
}

/* (b) renamed: the fd follows the directory, not its old name */
static int t_renamed(void)
{
	char a[96], b[96], f[128];
	snprintf(a, sizeof a, "%s/r1", B);
	snprintf(b, sizeof b, "%s/r2", B);
	if (mkdir(a, 0755)) return fail("mkdir r1");
	snprintf(f, sizeof f, "%s/f", a); put(f, "old");
	int fd = open(a, O_RDONLY | O_DIRECTORY);
	if (fd < 0) return fail("open r1");
	if (rename(a, b)) return fail("rename r1 r2");
	if (mkdir(a, 0755)) return fail("mkdir r1 again");
	put(f, "new");
	char s[16];
	get_at(fd, "f", s, sizeof s);
	if (strcmp(s, "old")) return fail(s[0] ? "renamed dir: read the new r1/f" : "renamed dir: f not found");
	if (mkdirat(fd, "sub", 0755)) return fail("mkdirat in renamed dir");
	struct stat st;
	char sub[128];
	snprintf(sub, sizeof sub, "%s/sub", b);
	if (stat(sub, &st)) return fail("mkdirat landed elsewhere");
	return 0;
}

/* (c) removed: nothing is found or made through it, not even in its namesake */
static int t_removed(void)
{
	char d[96], g[128];
	snprintf(d, sizeof d, "%s/rm", B);
	if (mkdir(d, 0755)) return fail("mkdir rm");
	int fd = open(d, O_RDONLY | O_DIRECTORY);
	if (fd < 0) return fail("open rm");
	if (rmdir(d)) return fail("rmdir rm");
	if (mkdir(d, 0755)) return fail("mkdir rm again");
	snprintf(g, sizeof g, "%s/g", d); put(g, "g");
	if (openat(fd, "g", O_RDONLY) >= 0) return fail("removed dir: found the new rm/g");
	if (openat(fd, "h", O_RDONLY | O_CREAT, 0644) >= 0) return fail("removed dir: created h");
	if (errno != ENOENT) return fail("removed dir: not ENOENT");
	return 0;
}

/* negative: chrooted with no held fd, every way up ends at the jail's root */
static int t_no_escape(void)
{
	char p[160];
	struct stat root, st;
	if (jail()) return fail("chroot jail");
	if (stat("/", &root)) return fail("stat /");
	if (stat("/..", &st) || !same(&st, &root)) return fail("/.. left the root");
	if (stat("../../../..", &st) || !same(&st, &root)) return fail("../../.. left the root");
	snprintf(p, sizeof p, "/../../..%s/secret", B);
	if (open(p, O_RDONLY) >= 0) return fail("absolute ../ path reached the secret");
	if (open("/esc_abs", O_RDONLY) >= 0) return fail("absolute symlink reached the secret");
	if (open("/esc_rel", O_RDONLY) >= 0) return fail("relative symlink reached the secret");
	int in = open("/in", O_RDONLY | O_DIRECTORY);
	if (in < 0) return fail("open /in");
	if (fstatat(in, "../../../..", &st, 0) || !same(&st, &root)) return fail("dirfd ../.. left the root");
	snprintf(p, sizeof p, "../../../..%s/secret", B);
	if (openat(in, p, O_RDONLY) >= 0) return fail("dirfd ../ path reached the secret");
	if (openat(in, "../esc_abs", O_RDONLY) >= 0) return fail("dirfd symlink reached the secret");
	/* through a bind mount: ".." leaves by the mountpoint, as the walk came in */
	int sub = open("/bmnt/sub", O_RDONLY | O_DIRECTORY);
	if (sub < 0) return fail("open /bmnt/sub");
	if (fstatat(sub, "../..", &st, 0) || !same(&st, &root)) return fail("bind dirfd ../.. is not the root");
	if (fstatat(sub, "../../../..", &st, 0) || !same(&st, &root)) return fail("bind dirfd left the root");
	return 0;
}

/* path p, if it resolves at all, is the jail's root (dir) and nothing above */
static int at_root(const char *p, const struct stat *root, const char *what)
{
	struct stat st;
	if (stat(p, &st) == 0 && !same(&st, root)) return fail(what);
	return 0;
}

/* negative: chrooted with no held fd and its own /proc, every magic link stays
 * inside, and the stat-only copies behind fd/N open and change nothing */
static int t_no_escape_proc(void)
{
	char p[200], m[240];
	struct stat root;
	if (unshare(CLONE_NEWNS)) return fail("unshare mount ns");
	if (jail()) return fail("chroot jail");
	if (stat("/", &root)) return fail("stat /");
	if (mount("proc", "/proc", "proc", 0, NULL)) return fail("mount proc in the jail");
	if (at_root("/proc/self/root", &root, "/proc/self/root is not the jail root") ||
	    at_root("/proc/self/root/..", &root, "/proc/self/root/.. left the root") ||
	    at_root("/proc/self/cwd/../..", &root, "/proc/self/cwd/../.. left the root") ||
	    at_root("/proc/1/root/..", &root, "/proc/1/root/.. left the root") ||
	    at_root("/proc/1/cwd/..", &root, "/proc/1/cwd/.. left the root")) return 1;
	snprintf(p, sizeof p, "/proc/self/root/../..%s/secret", B);
	if (open(p, O_RDONLY) >= 0) return fail("/proc/self/root reached the secret");
	snprintf(p, sizeof p, "/proc/1/root%s/secret", B);
	if (open(p, O_RDONLY) >= 0) return fail("/proc/1/root reached the secret");
	/* every fd of init's and our own: no file opened, written or chmodded through
	 * the copy behind it, and ".." past it stays inside */
	const char *dirs[] = { "/proc/1/fd", "/proc/self/fd" };
	for (int k = 0; k < 2; k++) {
		DIR *d = opendir(dirs[k]);
		if (!d) return fail("opendir fd directory");
		struct dirent *e;
		while ((e = readdir(d))) {
			if (e->d_name[0] == '.' || (k == 1 && atoi(e->d_name) == dirfd(d))) continue;
			snprintf(p, sizeof p, "%s/%s", dirs[k], e->d_name);
			int fd = open(p, O_RDONLY | O_NONBLOCK);
			snprintf(m, sizeof m, "%s: opened through it", p);
			if (fd >= 0) return fail(m);
			snprintf(m, sizeof m, "%s: chmod through it", p);
			if (chmod(p, 0777) == 0) return fail(m);
			snprintf(m, sizeof m, "%s/..", p);
			if (at_root(m, &root, "fd/N/.. left the root")) return 1;
		}
		closedir(d);
	}
	return 0;
}

/* stat through fd/N: of a pipe, a socket and a file, before and after chroot */
static int fdlink_kinds(int dfd, int pf, int sk, int rf, const char *when)
{
	char n[16], m[96];
	struct stat st;
	snprintf(n, sizeof n, "%d", pf);
	snprintf(m, sizeof m, "fstatat(self_fd, pipe) %s", when);
	if (fstatat(dfd, n, &st, 0) || !S_ISFIFO(st.st_mode)) return fail(m);
	snprintf(n, sizeof n, "%d", sk);
	snprintf(m, sizeof m, "fstatat(self_fd, socket) %s", when);
	if (fstatat(dfd, n, &st, 0) || !S_ISSOCK(st.st_mode)) return fail(m);
	snprintf(n, sizeof n, "%d", rf);
	snprintf(m, sizeof m, "fstatat(self_fd, file) %s", when);
	if (fstatat(dfd, n, &st, 0) || !S_ISREG(st.st_mode) || st.st_size != 1) return fail(m);
	return 0;
}

static int t_fdlink(void)
{
	int pp[2], sv[2];
	char p[128], s[16];
	if (pipe(pp) || socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return fail("pipe/socketpair");
	snprintf(p, sizeof p, "%s/secret", B);
	int rf = open(p, O_RDONLY);
	int pfd = open("/proc", O_RDONLY | O_DIRECTORY);
	int dfd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY);
	if (rf < 0 || pfd < 0 || dfd < 0) return fail("open");
	if (fdlink_kinds(dfd, pp[0], sv[0], rf, "before chroot")) return 1;
	/* a file of this root is still reopened by its path */
	snprintf(p, sizeof p, "/proc/self/fd/%d", rf);
	get_at(AT_FDCWD, p, s, sizeof s);
	if (strcmp(s, "s")) return fail("reopen through /proc/self/fd/N");
	if (chroot("/proc/self/fdinfo/") || chdir("/")) return fail("chroot fdinfo");
	int fd = openat(pfd, "self/fd/", O_RDONLY | O_DIRECTORY);
	if (fd < 0) return fail("openat(proc_fd, self/fd/) in the empty root");
	if (fdlink_kinds(fd, pp[0], sv[0], rf, "in the empty root")) return 1;
	snprintf(p, sizeof p, "%d", rf);
	if (openat(fd, p, O_RDWR) >= 0) return fail("the copy behind fd/N opened for writing");
	if (openat(fd, p, O_RDONLY) >= 0) return fail("the copy behind fd/N opened");
	if (fchmodat(fd, p, 0666, 0) == 0) return fail("chmod through the copy behind fd/N");
	struct stat st;
	if (fstatat(fd, p, &st, 0) || (st.st_mode & 07777) != 0644) return fail("the file's mode changed");
	return 0;
}

/* negative: nobody sees neither the link nor the file behind root's fd/N */
static int t_fdlink_other(void)
{
	char p[128], l[128];
	snprintf(p, sizeof p, "%s/secret", B);
	int rf = open(p, O_RDONLY);
	if (rf < 0) return fail("open secret");
	pid_t owner = getpid();
	pid_t c = fork();
	if (c == 0) {
		struct stat st;
		if (setgid(65534) || setuid(65534)) _exit(2);
		snprintf(p, sizeof p, "/proc/%d/fd/%d", (int)owner, rf);
		if (stat(p, &st) == 0) _exit(3);
		if (readlink(p, l, sizeof l) >= 0) _exit(4);
		_exit(0);
	}
	int st = 0;
	waitpid(c, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) == 2) return fail("setuid nobody");
	if (WEXITSTATUS(st) == 3) return fail("nobody stat root's fd/N");
	if (WEXITSTATUS(st) == 4) return fail("nobody readlink root's fd/N");
	return 0;
}

static int run(const char *name, int (*fn)(void))
{
	int pp[2];
	if (pipe(pp)) return 1;
	pid_t c = fork();
	if (c == 0) { close(pp[0]); msgfd = pp[1]; _exit(fn()); }
	close(pp[1]);
	char m[256];
	int n = (int)read(pp[0], m, sizeof m - 1);
	close(pp[0]);
	m[n > 0 ? n : 0] = 0;
	int st = 0;
	waitpid(c, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return 0;
	printf("FAIL %s: %s\n", name, m[0] ? m : "child died");
	return 1;
}

int main(void)
{
	char p[160], q[160];
	snprintf(B, sizeof B, "/tmp/dirfd.%d", (int)getpid());
	mkdir(B, 0755);
	snprintf(p, sizeof p, "%s/secret", B); put(p, "s");
	snprintf(p, sizeof p, "%s/jail", B); mkdir(p, 0755);
	snprintf(p, sizeof p, "%s/jail/in", B); mkdir(p, 0755);
	snprintf(p, sizeof p, "%s/jail/proc", B); mkdir(p, 0755);
	snprintf(p, sizeof p, "%s/jail/esc_abs", B);
	snprintf(q, sizeof q, "%s/secret", B);
	if (symlink(q, p)) { puts("FAIL symlink"); return 0; }
	snprintf(p, sizeof p, "%s/jail/esc_rel", B);
	if (symlink("../../../../../../secret", p)) { puts("FAIL symlink"); return 0; }
	snprintf(p, sizeof p, "%s/bsrc", B); mkdir(p, 0755);
	snprintf(p, sizeof p, "%s/bsrc/sub", B); mkdir(p, 0755);
	snprintf(q, sizeof q, "%s/jail/bmnt", B); mkdir(q, 0755);
	snprintf(p, sizeof p, "%s/bsrc", B);
	if (mount(p, q, NULL, MS_BIND, NULL)) { puts("FAIL bind mount"); return 0; }

	int bad = 0;
	bad |= run("proc fd, chroot jail", t_proc_jail);
	bad |= run("proc fd, empty root", t_proc_empty);
	bad |= run("held base fd", t_held_base);
	bad |= run("renamed dir", t_renamed);
	bad |= run("removed dir", t_removed);
	bad |= run("no escape without a held fd", t_no_escape);
	bad |= run("fd links name the open file", t_fdlink);
	bad |= run("another user's fd links", t_fdlink_other);
	bad |= run("no escape through the jail's own /proc", t_no_escape_proc);

	snprintf(p, sizeof p, "%s/secret", B);
	if (access(p, F_OK)) { puts("FAIL secret was moved"); bad = 1; }
	umount(q);
	puts(bad ? "FAIL" : "OK");
	return 0;
}
