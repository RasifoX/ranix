/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* open("/proc/self/fd/N") of a /proc file makes a second open file description
 * of the same in-memory inode. The first close used to free that inode while
 * the second still held it, and the second close freed it again: the heap's
 * "kfree bad ptr" (found by test_fuzz in the full suite). Both descriptions
 * must keep working until their own close, and the heap must stay quiet.
 * The open takes that reopen path only when the link's path no longer walks
 * (test_fuzz: a child that could not walk to its root parent's /proc/<pid>/mem);
 * here, sure every time, a /proc/<pid>/status kept open across the task's reaping.
 * The reopen is also the place nothing else checks: it must give no more than
 * the original open (mode, O_APPEND) nor than the caller's own permission, and
 * keep O_EXCL and O_DIRECTORY meaning what they mean. Another task's fds stay
 * hidden from a same-uid reader when that task is not dumpable.
 * Prints FAIL lines, then OK or FAIL. Runs as root (reads the kernel log). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/klog.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int bad_frees(void)
{
	int n = klogctl(10, NULL, 0);            /* SYSLOG_ACTION_SIZE_BUFFER */
	if (n <= 0) n = 1 << 18;
	char *b = malloc((size_t)n + 1);
	if (!b) return -1;
	int len = klogctl(3, b, n);              /* SYSLOG_ACTION_READ_ALL */
	int c = 0;
	if (len > 0) {
		b[len] = 0;
		for (char *p = b; (p = strstr(p, "kfree bad ptr")); p++) c++;
	}
	free(b);
	return c;
}

int main(void)
{
	const char *files[] = { "/proc/self/status", "/proc/meminfo", "/proc/version", "/sys/kernel/ostype" };
	int before = bad_frees();
	CHECK(before >= 0, "cannot read the kernel log");
	for (int r = 0; r < 20; r++) {
		for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) {
			int a = open(files[i], O_RDONLY);
			if (a < 0) continue;
			char path[64], buf[64];
			snprintf(path, sizeof path, "/proc/self/fd/%d", a);
			int b = open(path, O_RDONLY);
			CHECK(b >= 0, "reopen %s through %s", files[i], path);
			if (b < 0) { close(a); continue; }
			close(a);                            /* the first goes, the second stays usable */
			CHECK(read(b, buf, sizeof buf) > 0, "read %s after the first close", files[i]);
			close(b);
		}
	}
	/* the reopen path with a /proc file, as test_fuzz met it (its child could not
	 * walk to the root parent's /proc/<pid>/mem): a link whose path no longer
	 * leads anywhere. Here /proc/<child>/status, open while the child is reaped.
	 * The reopen makes a second description of the same in-memory inode; each
	 * close must drop only its own hold (the kernel log check below), and it
	 * gives no more than the original read-only open, root or not. */
	pid_t c;
	int st = 0, sync1[2];
	CHECK(pipe(sync1) == 0, "pipe");
	c = fork();
	if (c == 0) { char z; close(sync1[1]); read(sync1[0], &z, 1); _exit(0); }
	close(sync1[0]);
	char cpath[64];
	snprintf(cpath, sizeof cpath, "/proc/%d/status", (int)c);
	int gone = open(cpath, O_RDONLY);
	CHECK(gone >= 0, "open %s", cpath);
	close(sync1[1]);
	waitpid(c, NULL, 0);                     /* its /proc entry is gone, the open is not */
	if (gone >= 0) {
		char path[64];
		snprintf(path, sizeof path, "/proc/self/fd/%d", gone);
		int b = open(path, O_RDONLY);
		CHECK(b >= 0, "reopen of a reaped task's /proc file through %s: errno %d", path, errno);
		errno = 0;
		int w = open(path, O_RDWR);
		CHECK(w < 0 && errno == EACCES, "a read-only /proc file reopened O_RDWR: %d errno %d, want EACCES", w, errno);
		/* the first goes, the others stay; the old kernel freed the inode at each
		 * close (closing the original first keeps that a logged bad free, as
		 * test_fuzz saw it, and not a reopen of freed memory) */
		close(gone);
		if (w >= 0) close(w);
		if (b >= 0) close(b);
	}
	/* no more than the original open allowed: a read-only descriptor reopened for
	 * writing by an unprivileged child is EACCES (an unlinked root file),
	 * or a reopen would hand out write access */
	int ro = open("/tmp/procfd-ro", O_RDWR | O_CREAT | O_TRUNC, 0644);
	CHECK(ro >= 0 && write(ro, "root", 4) == 4, "make /tmp/procfd-ro");
	close(ro);
	ro = open("/tmp/procfd-ro", O_RDONLY);
	unlink("/tmp/procfd-ro");                /* only the descriptor names it now */
	c = fork();
	if (c == 0) {
		char path[64];
		if (setgid(65534) || setuid(65534)) _exit(9);
		int bad = 0;
		snprintf(path, sizeof path, "/proc/self/fd/%d", ro);
		int w = open(path, O_RDWR);
		if (w >= 0 || errno != EACCES) bad |= 1;
		if (w >= 0 && write(w, "user", 4) == 4) bad |= 2;
		_exit(bad);
	}
	waitpid(c, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "reopen gave more than the original open: status 0x%x (1 RDWR on an unlinked root file, 2 written)", st);
	char back[8] = "";
	CHECK(pread(ro, back, 4, 0) == 4 && !memcmp(back, "root", 4), "the root file was changed through a reopen");
	close(ro);
	/* O_APPEND kept, and the caller's own permission: root's append-only log
	 * (0600, unlinked so the child cannot walk to it) handed to an unprivileged child */
	int lg = open("/tmp/procfd-log", O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0600);
	CHECK(lg >= 0 && write(lg, "root", 4) == 4, "make /tmp/procfd-log");
	unlink("/tmp/procfd-log");
	c = fork();
	if (c == 0) {
		char path[64];
		if (setgid(65534) || setuid(65534)) _exit(9);
		snprintf(path, sizeof path, "/proc/self/fd/%d", lg);
		int bad = 0;
		int t1 = open(path, O_WRONLY | O_TRUNC);
		if (t1 >= 0 || errno != EACCES) bad |= 1;
		int t2 = open(path, O_WRONLY);
		if (t2 >= 0 || errno != EACCES) bad |= 2;
		int t3 = open(path, O_WRONLY | O_APPEND);   /* the right mode, but not the caller's file */
		if (t3 >= 0 || errno != EACCES) bad |= 4;
		_exit(bad);
	}
	waitpid(c, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "reopen of an append-only root log: status 0x%x (1 TRUNC, 2 no APPEND, 4 no permission check)", st);
	struct stat lst;
	CHECK(fstat(lg, &lst) == 0 && lst.st_size == 4, "the root log was truncated through a reopen (size %ld)", (long)lst.st_size);
	close(lg);

	/* O_EXCL and O_DIRECTORY keep their meaning on a reopen */
	int ux = open("/tmp/procfd-excl", O_RDWR | O_CREAT | O_TRUNC, 0600);
	unlink("/tmp/procfd-excl");
	char upath[64];
	snprintf(upath, sizeof upath, "/proc/self/fd/%d", ux);
	errno = 0;
	int e1 = open(upath, O_RDWR | O_CREAT | O_EXCL, 0600);
	CHECK(e1 < 0 && errno == EEXIST, "reopen with O_CREAT|O_EXCL: %d errno %d, want EEXIST", e1, errno);
	errno = 0;
	int e2 = open(upath, O_RDONLY | O_DIRECTORY);
	CHECK(e2 < 0 && errno == ENOTDIR, "reopen of a file with O_DIRECTORY: %d errno %d, want ENOTDIR", e2, errno);
	if (e1 >= 0) close(e1);
	if (e2 >= 0) close(e2);
	close(ux);

	/* a non-dumpable task's fds are hidden from another task of the same uid */
	int pfd[2];
	CHECK(pipe(pfd) == 0, "pipe");
	pid_t hid = fork();
	if (hid == 0) {
		if (setgid(65534) || setuid(65534)) _exit(9);
		prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
		close(pfd[0]);
		write(pfd[1], "r", 1);
		pause();
		_exit(0);
	}
	char r;
	close(pfd[1]);
	read(pfd[0], &r, 1);
	c = fork();
	if (c == 0) {
		char path[64], link[256];
		if (setgid(65534) || setuid(65534)) _exit(9);
		snprintf(path, sizeof path, "/proc/%d/fd/0", (int)hid);
		_exit(readlink(path, link, sizeof link) < 0 ? 0 : 1);
	}
	waitpid(c, &st, 0);
	kill(hid, SIGKILL);
	waitpid(hid, NULL, 0);
	close(pfd[0]);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a non-dumpable task's fd link was readable by another task of its uid");
	int after = bad_frees();
	CHECK(after == before, "%d bad frees in the kernel log", after - before);
	puts(fails ? "FAIL" : "OK");
	return fails != 0;
}
