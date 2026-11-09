/* CLONE_FS: tasks made with it share one root, working directory and umask
 * (Linux); without it each task has its own copy.
 *   1. chdir and umask by a CLONE_FS child are seen by the parent; by a plain
 *      fork child they are not
 *   2. chroot by a CLONE_FS child is seen by the parent (checked in a
 *      throwaway process, so the test itself keeps its root)
 *   3. unshare(CLONE_FS) in a CLONE_FS child: its chdir stays its own
 *   4. Chromium's pattern, as nobody in its own user namespace: a
 *      CLONE_FS|CLONE_VM|CLONE_VFORK child chroots into /proc/self/fdinfo/
 *      and exits; afterwards the parent cannot see /proc
 *   5. 1000 rounds of each kind of child leave no memory behind
 * Prints OK, or FAIL with what was wrong. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define NOBODY 65534
#define TOP    "/tmp/L_cf"

static char stack[64 * 1024] __attribute__((aligned(16)));

static int seen(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

static int put(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return -errno;
	int r = write(fd, s, strlen(s)) == (ssize_t)strlen(s) ? 0 : -errno;
	close(fd);
	return r;
}

/* run fn in a clone child with flags (plus SIGCHLD); its exit code */
static int clone_run(int (*fn)(void *), int flags, void *arg)
{
	pid_t c = clone(fn, stack + sizeof stack, flags | SIGCHLD, arg);
	if (c < 0) return 200;
	int st = 0;
	if (waitpid(c, &st, 0) != c) return 201;
	return WIFEXITED(st) ? WEXITSTATUS(st) : 202;
}

static int child_chdir(void *dir)  { return chdir((const char *)dir) ? 1 : 0; }
static int child_umask(void *arg)  { (void)arg; umask(077); return 0; }
static int child_chroot(void *dir) { return chroot((const char *)dir) ? 1 : 0; }
static int child_unshare_chdir(void *dir)
{
	if (unshare(CLONE_FS)) return 2;
	return chdir((const char *)dir) ? 1 : 0;
}
/* Chromium's ChrootToSelfFdinfo */
static int child_fdinfo(void *arg)
{
	(void)arg;
	if (chroot("/proc/self/fdinfo/")) _exit(1);
	if (chdir("/")) _exit(2);
	_exit(0);
}

static int cwd_is(const char *want)
{
	char b[512];
	return getcwd(b, sizeof b) && !strcmp(b, want);
}

static int share_cwd_umask(void)
{
	if (chdir("/")) { puts("FAIL chdir /"); return 1; }
	int r = clone_run(child_chdir, 0, TOP "/a");
	if (r || !cwd_is("/")) { printf("FAIL a plain child's chdir reached the parent (%d)\n", r); return 1; }
	r = clone_run(child_chdir, CLONE_FS, TOP "/a");
	if (r || !cwd_is(TOP "/a")) { printf("FAIL a CLONE_FS child's chdir did not reach the parent (%d)\n", r); return 1; }
	if (chdir("/")) return 1;
	umask(022);
	clone_run(child_umask, 0, NULL);
	mode_t m = umask(022);
	if (m != 022) { puts("FAIL a plain child's umask reached the parent"); return 1; }
	clone_run(child_umask, CLONE_FS, NULL);
	m = umask(022);
	if (m != 077) { printf("FAIL a CLONE_FS child's umask did not reach the parent (%o)\n", m); return 1; }
	r = clone_run(child_unshare_chdir, CLONE_FS, TOP "/a");
	if (r || !cwd_is("/")) { printf("FAIL chdir after unshare(CLONE_FS) reached the parent (%d)\n", r); return 1; }
	return 0;
}

/* in a throwaway process: does a child's chroot move this process's root? */
static int chroot_seen(int flags)
{
	pid_t c = fork();
	if (c == 0) {
		if (clone_run(child_chroot, flags, TOP "/jail")) _exit(3);
		_exit(seen("/inside") ? 1 : 0);
	}
	int st = 0;
	waitpid(c, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 4;
}

static int chromium_pattern(void)
{
	pid_t c = fork();
	if (c == 0) {
		if (setgid(NOBODY) || setuid(NOBODY)) _exit(10);
		if (unshare(CLONE_NEWUSER)) _exit(11);
		if (put("/proc/self/uid_map", "65534 65534 1\n") || put("/proc/self/gid_map", "65534 65534 1\n")) _exit(12);
		int r = clone_run(child_fdinfo, CLONE_FS | CLONE_VM | CLONE_VFORK, NULL);
		if (r) _exit(20 + r);
		if (seen("/proc")) _exit(13);          /* Credentials::HasFileSystemAccess */
		if (seen("/tmp") || seen("/bin")) _exit(14);
		_exit(0);
	}
	int st = 0;
	waitpid(c, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}

static long mem_free_kb(void)
{
	char b[256];
	int fd = open("/proc/meminfo", O_RDONLY);
	if (fd < 0) return -1;
	int n = (int)read(fd, b, sizeof b - 1);
	close(fd);
	if (n <= 0) return -1;
	b[n] = 0;
	char *s = strstr(b, "MemFree:");
	return s ? strtol(s + 8, NULL, 10) : -1;
}

/* 1000 rounds: plain child and CLONE_FS child, each chdir into a long path
 * (a fresh cwd string every time). A leaked fs_struct keeps its string: about
 * 4 MB over the run. */
static int no_leak(const char *longdir)
{
	for (int i = 0; i < 20; i++) { clone_run(child_chdir, 0, (void *)longdir); clone_run(child_chdir, CLONE_FS, (void *)longdir); }
	if (chdir("/")) return 1;
	long before = mem_free_kb();
	for (int i = 0; i < 1000; i++) {
		if (clone_run(child_chdir, 0, (void *)longdir)) { puts("FAIL leak round chdir"); return 1; }
		if (clone_run(child_chdir, CLONE_FS, (void *)longdir)) { puts("FAIL leak round CLONE_FS chdir"); return 1; }
		if (chdir("/")) return 1;
	}
	long after = mem_free_kb();
	printf("  MemFree before %ld kB, after %ld kB\n", before, after);
	if (before < 0 || after < 0) { puts("FAIL meminfo"); return 1; }
	if (before - after > 1024) { printf("FAIL 1000 clones kept %ld kB\n", before - after); return 1; }
	return 0;
}

int main(void)
{
	if (getuid() != 0) { puts("FAIL run as root"); return 0; }
	mkdir(TOP, 0755);
	if (mount("tmpfs", TOP, "tmpfs", 0, NULL)) { printf("FAIL mount %d\n", errno); return 0; }
	chmod(TOP, 0755);
	mkdir(TOP "/a", 0755);
	mkdir(TOP "/jail", 0755);
	close(open(TOP "/jail/inside", O_WRONLY | O_CREAT, 0644));
	/* a directory chain about 3000 bytes long */
	char longdir[3200];
	int n = snprintf(longdir, sizeof longdir, "%s", TOP);
	for (int d = 0; d < 14; d++) {
		longdir[n++] = '/';
		memset(longdir + n, 'a' + d, 200);
		n += 200;
		longdir[n] = 0;
		if (mkdir(longdir, 0755) && errno != EEXIST) { printf("FAIL mkdir long %d\n", errno); return 0; }
	}

	int fail = share_cwd_umask();
	if (!fail) {
		int a = chroot_seen(0), b = chroot_seen(CLONE_FS);
		if (a) { printf("FAIL a plain child's chroot reached the parent (%d)\n", a); fail = 1; }
		else if (b != 1) { printf("FAIL a CLONE_FS child's chroot did not reach the parent (%d)\n", b); fail = 1; }
	}
	if (!fail) {
		int c = chromium_pattern();
		if (c) { printf("FAIL Chromium chroot into /proc/self/fdinfo/ (step %d)\n", c); fail = 1; }
	}
	if (!fail) fail = no_leak(longdir);
	if (chdir("/")) { }
	umount(TOP);
	if (!fail) puts("OK");
	return 0;
}
