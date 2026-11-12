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
 * The reopen is also the place nothing else checks, and it checks what Linux's
 * may_open does: the caller's own permission on the file (DAC), not the mode of
 * the original open. So one's own file opened read-only reopens read-write, an
 * O_APPEND open reopens without O_APPEND, and O_CREAT (no O_EXCL) of the link of
 * an unlinked file opens that file; while root's file handed to an unprivileged
 * child still gives it no more than its own permission. O_EXCL and O_DIRECTORY
 * keep their meaning. Another task's fds stay hidden from a same-uid reader when
 * that task is not dumpable, and a task that changed its identity with
 * setuid/setgid is not dumpable (Linux commit_creds, suid_dumpable 0): the old
 * user sees neither its fds nor its memory (ptrace, process_vm_readv).
 * Prints FAIL lines, then OK or FAIL. Runs as root (reads the kernel log). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/fsuid.h>
#include <sys/klog.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/uio.h>
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

/* in every forked task at the same address: what an observer tries to read */
static volatile char secret_word[16] = "procfd-secret";

/* A task of uid 65534 looks at target: its fd link fdn, ptrace attach, and
 * process_vm_readv of secret_word. 0 when all are refused, else a bit for each
 * that got through (1 fd link, 2 attach, 4 memory); 0x80 when it could not run. */
static int observe(pid_t target, int fdn)
{
	pid_t o = fork();
	if (o == 0) {
		char path[64], link[256], got[16];
		int bad = 0;
		if (setgid(65534) || setuid(65534)) _exit(0x80);
		snprintf(path, sizeof path, "/proc/%d/fd/%d", (int)target, fdn);
		if (readlink(path, link, sizeof link) >= 0) bad |= 1;
		if (ptrace(PTRACE_ATTACH, target, 0, 0) == 0) {
			bad |= 2;
			waitpid(target, NULL, __WALL);
			ptrace(PTRACE_DETACH, target, 0, 0);
		}
		struct iovec l = { got, sizeof got }, r = { (void *)secret_word, sizeof got };
		if (process_vm_readv(target, &l, 1, &r, 1, 0) > 0) bad |= 4;
		_exit(bad);
	}
	int st = 0;
	waitpid(o, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 0x80;
}

/* Linux commit_creds: a change of the effective or filesystem uid or gid makes
 * the task not dumpable (suid_dumpable, 0 by default); one that changes nothing
 * leaves it. Otherwise a root task that opened a secret and dropped to 65534
 * is open to every other task of 65534: its fd links, ptrace, its memory. */
static void dumpable_on_setid(void)
{
	pid_t c = fork();
	if (c == 0) {
		int bad = 0;
		prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
		if (setresuid(-1, -1, -1) || prctl(PR_GET_DUMPABLE) != 1) bad |= 1;  /* no change */
		if (seteuid(65534) || prctl(PR_GET_DUMPABLE) != 0) bad |= 2;
		if (seteuid(0)) _exit(0x80);
		prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
		setfsuid(65534);
		if (prctl(PR_GET_DUMPABLE) != 0) bad |= 4;
		setfsuid(0);
		prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
		if (setegid(65534) || prctl(PR_GET_DUMPABLE) != 0) bad |= 8;
		_exit(bad);
	}
	int st = 0;
	waitpid(c, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "dumpable after an id change: status 0x%x (1 lowered by a no-op setresuid, 2 kept by seteuid, 4 by setfsuid, 8 by setegid)", st);

	/* root opens its 0600 secret, drops every id to 65534 and waits */
	int s = open("/tmp/procfd-secret", O_RDWR | O_CREAT | O_TRUNC, 0600);
	CHECK(s >= 0 && write(s, "root", 4) == 4, "make /tmp/procfd-secret");
	if (s >= 0) close(s);
	int pfd[2];
	CHECK(pipe(pfd) == 0, "pipe");
	pid_t t = fork();
	if (t == 0) {
		close(pfd[0]);
		int fd = open("/tmp/procfd-secret", O_RDONLY);
		if (fd < 0 || setgid(65534) || setuid(65534)) _exit(9);
		signed char m[2] = { (signed char)fd, (signed char)prctl(PR_GET_DUMPABLE) };
		write(pfd[1], m, 2);
		pause();
		_exit(0);
	}
	close(pfd[1]);
	signed char m[2] = { -1, -1 };
	CHECK(read(pfd[0], m, 2) == 2, "the dropping child did not start");
	close(pfd[0]);
	CHECK(m[1] == 0, "a root task that setuid() to 65534 is still dumpable (%d)", m[1]);
	if (m[0] >= 0) {
		int o = observe(t, m[0]);
		CHECK(o == 0, "a task that dropped root was open to the old user 65534: 0x%x (1 fd link of root's secret read, 2 ptrace attach, 4 process_vm_readv)", o);
	}
	kill(t, SIGKILL);
	waitpid(t, NULL, 0);
	unlink("/tmp/procfd-secret");
}

/* Linux semantics of the reopen: the caller's own permission, not the
 * original open's mode, and O_CREAT (no O_EXCL) of the link opens the file. */
static void reopen_linux(void)
{
	char path[64];
	struct stat a, b;
	/* creat-reopen: an unlinked file, O_CREAT without O_EXCL opens it, makes nothing */
	int x = open("/tmp/procfd-creat", O_RDWR | O_CREAT | O_TRUNC, 0600);
	CHECK(x >= 0, "make /tmp/procfd-creat");
	unlink("/tmp/procfd-creat");
	snprintf(path, sizeof path, "/proc/self/fd/%d", x);
	errno = 0;
	int y = open(path, O_RDWR | O_CREAT, 0600);
	CHECK(y >= 0, "creat-reopen: O_CREAT of %s (an unlinked file): errno %d, want the file", path, errno);
	if (y >= 0) {
		char back[4] = "";
		CHECK(fstat(x, &a) == 0 && fstat(y, &b) == 0 && a.st_ino == b.st_ino && a.st_dev == b.st_dev,
		      "creat-reopen opened another file");
		CHECK(write(y, "new", 3) == 3 && pread(x, back, 3, 0) == 3 && !memcmp(back, "new", 3),
		      "creat-reopen: written through the new fd, not read through the first");
		close(y);
	}
	CHECK(access("/tmp/procfd-creat", F_OK) != 0, "creat-reopen made the unlinked name again");
	close(x);

	/* own-ro-to-rw: 65534's own 0644 file, open read-only, reopened read-write */
	int ro = open("/tmp/procfd-own", O_RDWR | O_CREAT | O_TRUNC, 0644);
	CHECK(ro >= 0 && write(ro, "root", 4) == 4 && fchown(ro, 65534, 65534) == 0, "make /tmp/procfd-own");
	close(ro);
	ro = open("/tmp/procfd-own", O_RDONLY);
	unlink("/tmp/procfd-own");
	pid_t c = fork();
	if (c == 0) {
		if (setgid(65534) || setuid(65534)) _exit(0x80);
		snprintf(path, sizeof path, "/proc/self/fd/%d", ro);
		int w = open(path, O_RDWR);
		if (w < 0) _exit(1);
		_exit(pwrite(w, "user", 4, 0) == 4 ? 0 : 2);
	}
	int st = 0;
	waitpid(c, &st, 0);
	char back[8] = "";
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "own-ro-to-rw: 65534 could not reopen its own 0644 file read-write: status 0x%x (1 open, 2 write)", st);
	CHECK(pread(ro, back, 4, 0) == 4 && (!WIFEXITED(st) || WEXITSTATUS(st) || !memcmp(back, "user", 4)),
	      "own-ro-to-rw: the write did not reach the file");
	close(ro);

	/* own-append-drop: O_APPEND belongs to the open, not to the file */
	int ap = open("/tmp/procfd-app", O_RDWR | O_CREAT | O_TRUNC, 0644);
	CHECK(ap >= 0 && write(ap, "abcd", 4) == 4 && fchown(ap, 65534, 65534) == 0, "make /tmp/procfd-app");
	int rd = ap;
	ap = open("/tmp/procfd-app", O_WRONLY | O_APPEND);
	unlink("/tmp/procfd-app");
	c = fork();
	if (c == 0) {
		if (setgid(65534) || setuid(65534)) _exit(0x80);
		snprintf(path, sizeof path, "/proc/self/fd/%d", ap);
		int w = open(path, O_WRONLY);
		if (w < 0) _exit(1);
		if (fcntl(w, F_GETFL) & O_APPEND) _exit(4);
		_exit(pwrite(w, "X", 1, 0) == 1 ? 0 : 2);
	}
	waitpid(c, &st, 0);
	memset(back, 0, sizeof back);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "own-append-drop: 65534 could not reopen its own O_APPEND file without O_APPEND: status 0x%x (1 open, 2 pwrite, 4 still O_APPEND)", st);
	CHECK(pread(rd, back, 4, 0) == 4 && (!WIFEXITED(st) || WEXITSTATUS(st) || !memcmp(back, "Xbcd", 4)),
	      "own-append-drop: pwrite at 0 did not land at 0 (%.4s)", back);
	close(ap);
	close(rd);
}

/* /proc/<pid>/mem stays closed: root's descriptor of a root task's mem, handed
 * to a 65534 child, neither reopens nor reads, with the task alive (its every
 * access goes through the ptrace check) or reaped (the reopen path itself) */
static void mem_stays_closed(void)
{
	int pfd[2];
	CHECK(pipe(pfd) == 0, "pipe");
	pid_t t = fork();
	if (t == 0) { char z; close(pfd[1]); read(pfd[0], &z, 1); _exit(0); }
	close(pfd[0]);
	char mp[64];
	snprintf(mp, sizeof mp, "/proc/%d/mem", (int)t);
	int m = open(mp, O_RDONLY);
	char got[16] = "";
	CHECK(m >= 0 && pread(m, got, sizeof got, (off_t)(unsigned long)secret_word) == (ssize_t)sizeof got,
	      "root could not read a root child's %s: errno %d", mp, errno);
	for (int phase = 0; phase < 2 && m >= 0; phase++) {
		if (phase == 1) { close(pfd[1]); waitpid(t, NULL, 0); }
		pid_t c = fork();
		if (c == 0) {
			char path[64], buf[16];
			int bad = 0;
			if (setgid(65534) || setuid(65534)) _exit(0x80);
			snprintf(path, sizeof path, "/proc/self/fd/%d", m);
			int w = open(path, O_RDWR);
			if (w >= 0) bad |= 1;
			int r = open(path, O_RDONLY);
			if (r >= 0 && pread(r, buf, sizeof buf, (off_t)(unsigned long)secret_word) > 0) bad |= 2;
			if (pread(m, buf, sizeof buf, (off_t)(unsigned long)secret_word) > 0) bad |= 4;
			if (w >= 0 && pwrite(w, "x", 1, (off_t)(unsigned long)secret_word) > 0) bad |= 8;
			_exit(bad);
		}
		int st = 0;
		waitpid(c, &st, 0);
		CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "/proc/<pid>/mem of root's task opened to 65534 (%s): status 0x%x (1 reopen RDWR, 2 reopen+read, 4 inherited read, 8 write)",
		      phase ? "reaped" : "alive", st);
	}
	if (m >= 0) close(m);
	else { close(pfd[1]); waitpid(t, NULL, 0); }
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
	 * close must drop only its own hold (the kernel log check below). */
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
		/* root-proc-rw: as on Linux, root's DAC override opens the 0444 file for
		 * writing (the mode of the original open does not count); the write
		 * itself is the file's to refuse */
		errno = 0;
		int w = open(path, O_RDWR);
		CHECK(w >= 0, "root reopened a 0444 /proc file O_RDWR: errno %d, want success (DAC override)", errno);
		CHECK(w < 0 || write(w, "x", 1) < 0, "a write to a reaped task's /proc/<pid>/status succeeded");
		/* the first goes, the others stay; the old kernel freed the inode at each
		 * close (closing the original first keeps that a logged bad free, as
		 * test_fuzz saw it, and not a reopen of freed memory) */
		close(gone);
		if (w >= 0) close(w);
		if (b >= 0) close(b);
	}
	/* root's file is not widened by 65534: a read-only descriptor of root's 0644
	 * file (unlinked, so no walk checks it) reopened for writing by an
	 * unprivileged child is EACCES, because the child itself may not write the
	 * file (Linux may_open), or a reopen would hand out write access */
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
	/* the caller's own permission: root's append-only log (0600, unlinked so the
	 * child cannot walk to it) handed to an unprivileged child as O_WRONLY|O_APPEND
	 * reopens in no mode at all, with or without O_APPEND or O_TRUNC: the child
	 * may not write root's 0600 file */
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
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "reopen of an append-only root log: status 0x%x (1 TRUNC, 2 WRONLY, 4 WRONLY|APPEND: no permission check)", st);
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
	st = observe(hid, 0);
	kill(hid, SIGKILL);
	waitpid(hid, NULL, 0);
	close(pfd[0]);
	CHECK(st == 0, "a non-dumpable task was open to another task of its uid: 0x%x (1 fd link read, 2 ptrace attach, 4 process_vm_readv)", st);

	dumpable_on_setid();
	reopen_linux();
	mem_stays_closed();
	int after = bad_frees();
	CHECK(after == before, "%d bad frees in the kernel log", after - before);
	puts(fails ? "FAIL" : "OK");
	return fails != 0;
}
