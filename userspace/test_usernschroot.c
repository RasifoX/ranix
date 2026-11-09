/* chroot needs CAP_SYS_CHROOT in the caller's own user namespace (Linux), so a
 * rootless sandbox (Chromium, unshare -r) may chroot, and nobody else may.
 *   1. nobody outside any namespace: EPERM
 *   2. root that dropped CAP_SYS_CHROOT: EPERM
 *   3. nobody in a user namespace of its own: allowed, and no way out of the
 *      jail by '..', by a symlink pointing up, or by a nested chroot (chroot
 *      resets the working directory, so the classic nested escape of Linux,
 *      chroot into a subdirectory and climb from the old cwd, is closed too)
 * Prints OK, or FAIL with what was wrong. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define NOBODY 65534
#define TOP    "/tmp/L_uc"
#define JAIL   TOP "/jail"
#define MARK   "L_uc_outside"

static int put(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0) return -errno;
	int r = write(fd, s, strlen(s)) == (ssize_t)strlen(s) ? 0 : -errno;
	close(fd);
	return r;
}

static int seen(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

/* run fn in a child; its exit code */
static int child(int (*fn)(void))
{
	pid_t c = fork();
	if (c == 0) _exit(fn());
	int st = 0;
	waitpid(c, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}

static int nobody_plain(void)
{
	if (setgid(NOBODY) || setuid(NOBODY)) return 2;
	return chroot(JAIL) == 0 ? 1 : (errno == EPERM ? 0 : 3);
}

static int root_without_cap(void)
{
	struct { unsigned v; int pid; } hdr = { 0x20080522, 0 };
	unsigned data[6] = { 0 };
	if (syscall(SYS_capget, &hdr, data)) return 2;
	data[0] &= ~(1u << 18);   /* CAP_SYS_CHROOT out of effective */
	if (syscall(SYS_capset, &hdr, data)) return 3;
	return chroot(JAIL) == 0 ? 1 : (errno == EPERM ? 0 : 4);
}

static int nobody_userns(void)
{
	if (setgid(NOBODY) || setuid(NOBODY)) return 2;
	if (unshare(CLONE_NEWUSER)) return 3;
	if (put("/proc/self/uid_map", "0 65534 1\n") || put("/proc/self/gid_map", "0 65534 1\n")) return 4;
	if (chdir(JAIL "/d1/d2")) return 5;
	if (chroot(JAIL)) return 6;
	if (!seen("/inside")) return 7;                       /* the jail is the root */
	if (seen("/" MARK) || seen("/../" MARK) || seen("../../../" MARK) ||
	    seen("/../../../../" MARK)) return 8;               /* '..' stops at the root */
	if (chdir("/d1") || chdir("../../..") || seen(MARK)) return 9;
	if (seen("/up/" MARK) || seen("/abs/" MARK)) return 10;  /* symlinks resolve inside */
	if (chroot("/d1") || chroot("..") || seen("/" MARK) || seen("/../../" MARK)) return 11;
	return 0;
}

int main(void)
{
	if (getuid() != 0) { puts("FAIL run as root"); return 0; }
	mkdir(TOP, 0755);
	if (mount("tmpfs", TOP, "tmpfs", 0, NULL)) { printf("FAIL mount %d\n", errno); return 0; }
	chmod(TOP, 0755);
	mkdir(JAIL, 0755);
	mkdir(JAIL "/d1", 0755);
	mkdir(JAIL "/d1/d2", 0755);
	close(open(TOP "/" MARK, O_WRONLY | O_CREAT, 0644));
	close(open(JAIL "/inside", O_WRONLY | O_CREAT, 0644));
	if (symlink("../..", JAIL "/up") || symlink(TOP, JAIL "/abs")) { printf("FAIL symlink %d\n", errno); return 0; }

	int a = child(nobody_plain), b = child(root_without_cap), c = child(nobody_userns);
	printf("  nobody=%d root-nocap=%d userns=%d\n", a, b, c);
	umount(TOP);
	if (a) { printf("FAIL nobody chrooted without the capability (%d)\n", a); return 0; }
	if (b) { printf("FAIL root without CAP_SYS_CHROOT chrooted (%d)\n", b); return 0; }
	if (c) { printf("FAIL user namespace chroot (step %d)\n", c); return 0; }
	puts("OK");
	return 0;
}
