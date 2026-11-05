/* File data in tmpfs and ramfs goes back to the allocator: on unlink, on overwrite
 * by rename, on umount and when a session's private /tmp ends, whatever is left
 * open or mapped at that moment. Each loop moves far more data than the machine
 * has free, so anything leaked at scale ends in ENOSPC or an OOM kill long before
 * the drift check; the check catches the small leaks. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include "asess_test.h"

#define MIB       (1024L * 1024)
#define CHUNK     (MIB)
#define DRIFT_KB  1024L     /* per-CPU page caches and the heap move this much on their own */

static char chunk[CHUNK];

static long free_kb(void)
{
	char b[256];
	int fd = open("/proc/meminfo", O_RDONLY);
	if (fd < 0) return -1;
	ssize_t n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0) return -1;
	b[n] = 0;
	char *p = strstr(b, "MemFree:");
	return p ? strtol(p + 8, NULL, 10) : -1;
}

static int fill(const char *path, int mib)
{
	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) return -1;
	for (int i = 0; i < mib; i++)
		if (write(fd, chunk, CHUNK) != CHUNK) { close(fd); return -1; }
	close(fd);
	return 0;
}

/* round() runs n times; free memory must end where it stood after warm rounds */
static void stable(const char *what, void (*round)(int), int n)
{
	long base = 0;
	for (int i = 0; i < n; i++) {
		round(i);
		if (i == 2) base = free_kb();
		if (fails) { printf("FAIL %s stopped at round %d\n", what, i); return; }
	}
	long end = free_kb();
	printf("%s: %d rounds, free %ld -> %ld KB\n", what, n, base, end);
	if (base - end > DRIFT_KB) { printf("FAIL %s leaks %ld KB\n", what, base - end); fails++; }
}

static const char *fs_name;

static void rm_round(int i)
{
	(void)i;
	CHECK(fill("/tmp/tf/a", 16) == 0, "fill");
	CHECK(fill("/tmp/tf/b", 16) == 0, "fill");
	CHECK(link("/tmp/tf/b", "/tmp/tf/c") == 0, "link");
	CHECK(unlink("/tmp/tf/a") == 0 && unlink("/tmp/tf/b") == 0 && unlink("/tmp/tf/c") == 0, "unlink");
}

static void rename_round(int i)
{
	(void)i;
	CHECK(fill("/tmp/tf/new", 16) == 0, "fill");
	CHECK(fill("/tmp/tf/old", 16) == 0, "fill");
	CHECK(rename("/tmp/tf/new", "/tmp/tf/old") == 0, "rename over");
	char c;
	int fd = open("/tmp/tf/old", O_RDONLY);
	CHECK(fd >= 0 && read(fd, &c, 1) == 1, "renamed file readable");
	close(fd);
	CHECK(unlink("/tmp/tf/old") == 0, "unlink");
}

static void tree_round(int i)
{
	(void)i;
	CHECK(mount("t", "/tmp/tf", fs_name, 0, strcmp(fs_name, "tmpfs") ? NULL : "size=128M") == 0, "mount");
	CHECK(mkdir("/tmp/tf/d", 0755) == 0 && mkdir("/tmp/tf/d/e", 0755) == 0, "mkdir");
	CHECK(fill("/tmp/tf/d/e/f", 16) == 0, "fill");
	CHECK(fill("/tmp/tf/g", 16) == 0, "fill");
	CHECK(link("/tmp/tf/g", "/tmp/tf/d/h") == 0, "link");
	CHECK(symlink("/tmp/tf/g", "/tmp/tf/d/s") == 0, "symlink");
	CHECK(mkfifo("/tmp/tf/d/p", 0644) == 0, "mkfifo");
	int fd = open("/tmp/tf/d/p", O_RDWR);
	close(fd);
	CHECK(mkdir("/tmp/tf/gone", 0755) == 0 && rmdir("/tmp/tf/gone") == 0, "rmdir");
	CHECK(umount("/tmp/tf") == 0, "umount");
}

/* an open file keeps its data past the unlink and the umount; the last close frees it */
static void open_round(int i)
{
	(void)i;
	CHECK(mount("t", "/tmp/tf", fs_name, 0, strcmp(fs_name, "tmpfs") ? NULL : "size=64M") == 0, "mount");
	CHECK(fill("/tmp/tf/a", 16) == 0 && fill("/tmp/tf/b", 16) == 0, "fill");
	int gone = open("/tmp/tf/a", O_RDWR), kept = open("/tmp/tf/b", O_RDONLY);
	int dir = open("/tmp/tf", O_RDONLY);
	CHECK(gone >= 0 && kept >= 0 && dir >= 0, "open");
	CHECK(unlink("/tmp/tf/a") == 0, "unlink");
	CHECK(umount("/tmp/tf") == 0, "umount with open files");
	char c = 1;
	CHECK(lseek(gone, 16 * MIB - 1, SEEK_SET) > 0 && read(gone, &c, 1) == 1 && c == 'r', "read after umount");
	CHECK(lseek(kept, 3 * MIB, SEEK_SET) > 0 && read(kept, &c, 1) == 1 && c == 'r', "read after umount");
	CHECK(lseek(gone, 0, SEEK_SET) == 0 && write(gone, "w", 1) == 1, "write after umount");
	close(gone); close(kept); close(dir);
}

/* a shared mapping keeps its pages past the unlink, the close and the umount, and
 * forked copies hold them too */
static void mapped_round(int i)
{
	CHECK(mount("t", "/tmp/tf", fs_name, 0, strcmp(fs_name, "tmpfs") ? NULL : "size=64M") == 0, "mount");
	CHECK(fill("/tmp/tf/a", 8) == 0, "fill");
	int fd = open("/tmp/tf/a", O_RDWR);
	char *m = mmap(NULL, 8 * MIB, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(fd >= 0 && m != MAP_FAILED, "mmap");
	pid_t p = fork();
	if (p == 0) {
		close(fd);
		CHECK(unlink("/tmp/tf/a") == 0, "unlink");
		_exit(fails);
	}
	waitpid(p, NULL, 0);
	close(fd);
	CHECK(umount("/tmp/tf") == 0, "umount under a mapping");
	for (long off = 0; off < 8 * MIB; off += 4096) {
		if (m[off] != 'r') { printf("FAIL page %ld lost\n", off); fails++; break; }
		m[off] = 'x';
	}
	p = fork();
	if (p == 0) {
		m[4096] = 'y';   /* a shared store from the child, seen by the parent */
		_exit(0);
	}
	waitpid(p, NULL, 0);
	CHECK(m[4096] == 'y', "shared store across fork");
	if (i & 1) {
		CHECK(munmap(m, 8 * MIB) == 0, "munmap");
		return;
	}
	/* or the last task to hold it just exits */
	p = fork();
	if (p == 0) _exit(0);
	waitpid(p, NULL, 0);
	CHECK(munmap(m, 8 * MIB) == 0, "munmap");
}

static void memfd_round(int i)
{
	(void)i;
	int fd = (int)syscall(SYS_memfd_create, "t", 0);
	CHECK(fd >= 0, "memfd_create");
	for (int k = 0; k < 16; k++)
		CHECK(write(fd, chunk, CHUNK) == CHUNK, "memfd write");
	char *m = fails ? MAP_FAILED : mmap(NULL, 16 * MIB, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED) { printf("FAIL memfd mmap\n"); fails++; return; }
	CHECK(m[12 * MIB] == 'r', "memfd mapped past close");
	CHECK(munmap(m, 16 * MIB) == 0, "munmap");
}

static void session_round(int i)
{
	(void)i;
	long sfd = sess_create("/", UID, NULL, 0, 0);
	CHECK(sfd >= 0, "create");
	long pid = sess_must_spawn(sfd);
	if (pid == 0) {
		int bad = fill("/tmp/d", 16) != 0 || mkdir("/tmp/s", 0755) != 0 ||
		          fill("/tmp/s/e", 16) != 0;
		int fd = open("/dev/null", O_WRONLY);   /* left open for teardown */
		(void)fd;
		int pfd[2];
		pipe(pfd);
		_exit(bad);
	}
	sess_reap((pid_t)pid);
	close((int)sfd);
}

static void many_files_round(int i)
{
	char p[64];
	CHECK(mount("t", "/tmp/tf", fs_name, 0, strcmp(fs_name, "tmpfs") ? NULL : "size=16M") == 0, "mount");
	for (int k = 0; k < 500; k++) {
		snprintf(p, sizeof(p), "/tmp/tf/%d-%d", i, k);
		int fd = open(p, O_CREAT | O_WRONLY, 0644);
		CHECK(fd >= 0 && write(fd, "x", 1) == 1, "small file");
		close(fd);
	}
	CHECK(umount("/tmp/tf") == 0, "umount");
}

static void one_fs(const char *fs)
{
	char name[64];
	fs_name = fs;
	CHECK(mount("t", "/tmp/tf", fs, 0, strcmp(fs, "tmpfs") ? NULL : "size=128M") == 0, "mount");
	snprintf(name, sizeof(name), "%s unlink", fs);
	stable(name, rm_round, 7);
	snprintf(name, sizeof(name), "%s rename", fs);
	stable(name, rename_round, 7);
	CHECK(umount("/tmp/tf") == 0, "umount");
	snprintf(name, sizeof(name), "%s umount", fs);
	stable(name, tree_round, 7);
	snprintf(name, sizeof(name), "%s open", fs);
	stable(name, open_round, 7);
	snprintf(name, sizeof(name), "%s mapped", fs);
	stable(name, mapped_round, 7);
	snprintf(name, sizeof(name), "%s files", fs);
	stable(name, many_files_round, 6);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	memset(chunk, 'r', sizeof(chunk));
	mkdir("/tmp/tf", 0755);
	/* first: many small files scatter the heap, and a file is one contiguous block */
	stable("memfd", memfd_round, 12);
	stable("session", session_round, 16);
	one_fs("tmpfs");
	one_fs("ramfs");
	return finish();
}
