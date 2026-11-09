/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* clone(CLONE_NEWNS) must give the child its own mount namespace, exactly as
 * unshare(CLONE_NEWNS) does (test_mountns covers unshare). Container and
 * sandbox tools create the namespace with clone; if the child silently keeps
 * the parent's namespace, every mount it makes lands on the host.
 * The child mounts a tmpfs and writes a marker while the parent looks, then
 * the parent checks again after the child is gone. CLONE_NEWNS with CLONE_FS
 * (a shared root across two mount tables) is EINVAL, as on Linux.
 * Prints FAIL lines, then OK or FAIL <n>. Runs as root. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define DIR_   "/tmp/clonens_mnt"
#define MARKER DIR_ "/marker"

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (errno=%d)\n", what, errno); fails++; } } while (0)

int main(void)
{
	if (geteuid() != 0) { puts("FAIL run as root"); return 0; }
	mkdir(DIR_, 0755);
	unlink(MARKER);
	int ready[2], done[2];
	if (pipe(ready) || pipe(done)) { puts("FAIL pipe"); return 0; }

	/* fork-style clone: no new stack, the child returns 0 here */
	long c = syscall(SYS_clone, (unsigned long)(CLONE_NEWNS | SIGCHLD), 0L, 0L, 0L, 0L);
	if (c < 0) { printf("FAIL clone(CLONE_NEWNS) errno=%d\n", errno); return 0; }
	if (c == 0) {
		char b = 'x';
		close(ready[0]);
		close(done[1]);
		mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);   /* no propagation back (Linux) */
		if (mount("tmpfs", DIR_, "tmpfs", 0, NULL) != 0) b = 'm';
		else {
			int fd = open(MARKER, O_CREAT | O_WRONLY, 0644);
			if (fd < 0) b = 'o'; else close(fd);
		}
		write(ready[1], &b, 1);
		read(done[0], &b, 1);                           /* hold the mount while the parent looks */
		_exit(0);
	}
	close(ready[1]);
	close(done[0]);
	char b = 0;
	CHECK(read(ready[0], &b, 1) == 1 && b == 'x', "child could not mount and write its marker");

	struct stat st;
	errno = 0;
	CHECK(stat(MARKER, &st) == -1 && errno == ENOENT, "the clone(CLONE_NEWNS) child's mount is visible to the parent");
	close(done[1]);
	waitpid((pid_t)c, NULL, 0);
	errno = 0;
	CHECK(stat(MARKER, &st) == -1 && errno == ENOENT, "the child's mount outlived the child in the parent's namespace");

	/* if it leaked, take it down so the next tests see a clean /tmp */
	if (stat(MARKER, &st) == 0) { unlink(MARKER); umount(DIR_); }
	rmdir(DIR_);

	errno = 0;
	long e = syscall(SYS_clone, (unsigned long)(CLONE_NEWNS | CLONE_FS | SIGCHLD), 0L, 0L, 0L, 0L);
	if (e == 0) _exit(0);
	if (e > 0) waitpid((pid_t)e, NULL, 0);
	CHECK(e == -1 && errno == EINVAL, "clone(CLONE_NEWNS | CLONE_FS) allowed");
	if (fails) printf("FAIL %d\n", fails); else puts("OK");
	return 0;
}
