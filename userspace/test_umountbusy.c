/* umount of a disk filesystem that is still in use. While a file or directory on
 * it is open, or a page of it is mapped, umount must say EBUSY and leave the
 * filesystem working; once the last user is gone umount goes through and the
 * device mounts again. A namespace copy keeps its own view alive after the
 * original umount. Found by code reading (2026-11-09): ext2 freed its in-memory
 * state on umount while files were still open.
 *
 *   test_umountbusy DEV MNT [CASE]   DEV ext2 at MNT with MNT/marker.txt
 *
 * Cases (all by default, in this order):
 *   open-busy   an open file: umount EBUSY, the fd still reads, close, umount, mount
 *   dir-busy    an open directory: the same
 *   mmap-busy   a 4 KB page mapped and the fd closed: EBUSY, the map still reads
 *   nobody      umount as nobody: EPERM (the door stays shut)
 *   race        a child opens and closes in a loop while umount and mount repeat
 *   ns-copy     a child in an unshared mount namespace still reads MNT after the
 *               parent's umount
 * A file left open after a wrong umount is parked in a child that never exits,
 * so the old kernel is not made to touch the freed filesystem on close.
 * Prints one PASS/FAIL line a case, then OK or FAIL. Runs as root. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define NOBODY 65534

static const char *g_dev, *g_mnt;
static int g_fail;

static void result(const char *name, int ok, const char *why)
{
	printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, ok ? "" : ": ", ok ? "" : why);
	if (!ok) g_fail = 1;
}

static int do_mount(void)
{
	return mount(g_dev, g_mnt, "ext2", 0, NULL);
}

/* MNT/marker.txt read fresh: 1 if it holds the marker */
static int marker_ok(void)
{
	char p[300], b[64] = "";
	snprintf(p, sizeof p, "%s/marker.txt", g_mnt);
	int fd = open(p, O_RDONLY);
	if (fd < 0) return 0;
	ssize_t n = read(fd, b, sizeof b - 1);
	close(fd);
	return n > 0 && strncmp(b, "ranix-nvme-ext2-ok", 18) == 0;
}

/* A child that opens path (O_DIRECTORY if dir), maps the first page if map, then
 * waits: 'c' closes (keeping the map), 'u' unmaps, 'q' exits, 'p' parks it for
 * good. Answers each command with one byte: '1' done, '0' failed. */
typedef struct { pid_t pid; int cmd, ans; } holder_t;

static void holder_loop(int cmd, int ans, const char *path, int dir, int map)
{
	int fd = open(path, dir ? O_RDONLY | O_DIRECTORY : O_RDONLY);
	char *m = MAP_FAILED;
	if (fd >= 0 && map) m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
	char ok = (fd >= 0 && (!map || m != MAP_FAILED)) ? '1' : '0';
	if (write(ans, &ok, 1) != 1) _exit(1);
	char c;
	while (read(cmd, &c, 1) == 1) {
		if (c == 'r') {             /* read through the fd, or the map */
			char b[8] = "";
			ok = map ? (m[0] == 'U' ? '1' : '0')
			         : (dir ? '1' : (pread(fd, b, 5, 0) == 5 && !strncmp(b, "ranix", 5) ? '1' : '0'));
		} else if (c == 'c') {
			ok = close(fd) == 0 ? '1' : '0';
			fd = -1;
		} else if (c == 'u') {
			ok = munmap(m, 4096) == 0 ? '1' : '0';
		} else if (c == 'p') {
			for (;;) pause();
		} else {
			_exit(0);
		}
		if (write(ans, &ok, 1) != 1) _exit(1);
	}
	_exit(0);
}

static int holder_start(holder_t *h, const char *path, int dir, int map)
{
	int c[2], a[2];
	if (pipe(c) || pipe(a)) return -1;
	h->pid = fork();
	if (h->pid == 0) {
		close(c[1]); close(a[0]);
		int nul = open("/dev/null", O_RDWR);   /* never hold the caller's stdout */
		if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
		holder_loop(c[0], a[1], path, dir, map);
	}
	close(c[0]); close(a[1]);
	h->cmd = c[1]; h->ans = a[0];
	char ok = 0;
	if (read(h->ans, &ok, 1) != 1 || ok != '1') return -1;
	return 0;
}

static int holder_do(holder_t *h, char c)
{
	char ok = 0;
	if (write(h->cmd, &c, 1) != 1) return 0;
	if (c == 'p') return 1;
	return read(h->ans, &ok, 1) == 1 && ok == '1';
}

static void holder_end(holder_t *h)
{
	holder_do(h, 'q');
	waitpid(h->pid, NULL, 0);
	close(h->cmd); close(h->ans);
}

/* After a umount that should not have happened: park the holder, mount again. */
static void wrongly_unmounted(holder_t *h)
{
	holder_do(h, 'p');
	close(h->cmd); close(h->ans);
	do_mount();
}

/* open, dir and mmap cases share one shape */
static void busy_case(const char *name, const char *path, int dir, int map)
{
	char why[160];
	holder_t h;
	if (holder_start(&h, path, dir, map) != 0) { result(name, 0, "could not open"); return; }
	if (map && !holder_do(&h, 'c')) { result(name, 0, "close after mmap failed"); holder_end(&h); return; }
	int r = umount(g_mnt), e = errno;
	if (r == 0) { wrongly_unmounted(&h); result(name, 0, "umount succeeded while in use"); return; }
	if (e != EBUSY) {
		snprintf(why, sizeof why, "umount gave errno %d, want EBUSY", e);
		holder_end(&h); result(name, 0, why); return;
	}
	if (!holder_do(&h, 'r')) { holder_end(&h); result(name, 0, "no longer readable after EBUSY"); return; }
	if (!holder_do(&h, map ? 'u' : 'c')) { holder_end(&h); result(name, 0, "release failed"); return; }
	holder_end(&h);
	if (umount(g_mnt) != 0) {
		snprintf(why, sizeof why, "umount after the last user failed, errno %d", errno);
		result(name, 0, why); return;
	}
	if (do_mount() != 0) { result(name, 0, "mount again failed"); return; }
	result(name, marker_ok(), "marker unreadable after mount again");
}

static void case_nobody(void)
{
	pid_t p = fork();
	if (p == 0) {
		if (setgid(NOBODY) || setuid(NOBODY)) _exit(9);
		_exit(umount(g_mnt) == 0 ? 0 : (errno == EPERM ? 1 : 2));
	}
	int st = 0;
	waitpid(p, &st, 0);
	int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 99;
	if (rc == 0) do_mount();
	result("nobody", rc == 1 && marker_ok(), rc == 0 ? "nobody unmounted it" : "not EPERM");
}

/* Opens racing umount: whatever the interleaving, the kernel stays up and the
 * filesystem mounts again at the end. */
static void case_race(void)
{
	char path[300];
	snprintf(path, sizeof path, "%s/marker.txt", g_mnt);
	pid_t p = fork();
	if (p == 0) {
		for (int i = 0; i < 4000; i++) {
			int fd = open(path, O_RDONLY);
			char b[8];
			if (fd >= 0) { if (read(fd, b, sizeof b) < 0) { } close(fd); }
		}
		_exit(0);
	}
	int unmounted = 0, busy = 0;
	for (int i = 0; i < 200; i++) {
		if (umount(g_mnt) == 0) { unmounted++; if (do_mount() != 0) break; }
		else if (errno == EBUSY) busy++;
	}
	int st = 0;
	waitpid(p, &st, 0);
	if (!marker_ok()) do_mount();
	char why[80];
	snprintf(why, sizeof why, "child status %d, %d unmounts, %d busy", st, unmounted, busy);
	result("race", WIFEXITED(st) && WEXITSTATUS(st) == 0 && marker_ok(), why);
}

static void case_ns_copy(void)
{
	int go[2], back[2];
	if (pipe(go) || pipe(back)) { result("ns-copy", 0, "pipe"); return; }
	pid_t p = fork();
	if (p == 0) {
		char c = unshare(CLONE_NEWNS) == 0 ? '1' : '0';
		if (write(back[1], &c, 1) != 1 || c != '1') _exit(1);
		if (read(go[0], &c, 1) != 1) _exit(1);
		c = marker_ok() ? '1' : '0';
		if (write(back[1], &c, 1) != 1) _exit(1);
		_exit(0);
	}
	char c = 0;
	if (read(back[0], &c, 1) != 1 || c != '1') { result("ns-copy", 0, "unshare failed"); waitpid(p, NULL, 0); return; }
	int r = umount(g_mnt), e = errno;
	if (write(go[1], "g", 1) != 1) { }
	c = 0;
	int seen = read(back[0], &c, 1) == 1 && c == '1';
	waitpid(p, NULL, 0);
	int again = (r == 0 ? do_mount() == 0 : 1) && marker_ok();
	char why[96];
	snprintf(why, sizeof why, "umount %d errno %d, child read %s, mount again %s",
	         r, r ? e : 0, seen ? "ok" : "failed", again ? "ok" : "failed");
	result("ns-copy", r == 0 && seen && again, why);
}

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: test_umountbusy DEV MNT [CASE]\n"); return 2; }
	g_dev = argv[1];
	g_mnt = argv[2];
	const char *only = argc > 3 ? argv[3] : NULL;
	setvbuf(stdout, NULL, _IONBF, 0);
	if (!marker_ok()) { printf("FAIL setup: %s/marker.txt not there\nFAIL\n", g_mnt); return 1; }

	char file[300], bin[300];
	snprintf(file, sizeof file, "%s/marker.txt", g_mnt);
	snprintf(bin, sizeof bin, "%s/um.bin", g_mnt);
	int fd = open(bin, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	char page[4096];
	memset(page, 'U', sizeof page);
	if (fd < 0 || write(fd, page, sizeof page) != (ssize_t)sizeof page) { printf("FAIL setup: um.bin\nFAIL\n"); return 1; }
	close(fd);
	sync();

#define RUN(n, call) if (!only || !strcmp(only, n)) { call; }
	RUN("open-busy", busy_case("open-busy", file, 0, 0));
	RUN("dir-busy",  busy_case("dir-busy", g_mnt, 1, 0));
	RUN("mmap-busy", busy_case("mmap-busy", bin, 0, 1));
	RUN("nobody",    case_nobody());
	RUN("race",      case_race());
	RUN("ns-copy",   case_ns_copy());
	unlink(bin);
	printf("%s\n", g_fail ? "FAIL" : "OK");
	return g_fail;
}
