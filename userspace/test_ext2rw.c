/* A compiler's use of a disk filesystem, in the directory given as argv[1]:
 * rename over a target, chmod and mtime that stick, hard and symbolic links,
 * unlink while open, truncate, holes, private and shared file mappings, and
 * several processes creating files in one directory at once. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
static int first_bad, first_errno;
#define CHECK(c) do { if (!(c)) { if (!fails++) { first_bad = __LINE__; first_errno = errno; } } } while (0)

static void put(const char *p, const char *s)
{
	int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	CHECK(fd >= 0);
	CHECK(write(fd, s, strlen(s)) == (ssize_t)strlen(s));
	close(fd);
}

static int same(const char *p, const char *s)
{
	char b[256];
	int fd = open(p, O_RDONLY);
	if (fd < 0) return 0;
	ssize_t n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n < 0) return 0;
	b[n] = 0;
	return strcmp(b, s) == 0;
}

static void names(void)
{
	struct stat st;
	time_t now = time(NULL);

	put("a", "first");
	CHECK(stat("a", &st) == 0 && st.st_size == 5);
	CHECK(st.st_mtime + 5 >= now);
	CHECK(chmod("a", 0751) == 0);
	CHECK(stat("a", &st) == 0 && (st.st_mode & 07777) == 0751);

	put("b", "second-longer");
	CHECK(rename("a", "b") == 0);
	CHECK(same("b", "first"));
	CHECK(access("a", F_OK) != 0 && errno == ENOENT);

	CHECK(link("b", "c") == 0);
	CHECK(stat("c", &st) == 0 && st.st_nlink == 2);
	CHECK(unlink("b") == 0);
	CHECK(same("c", "first"));
	CHECK(stat("c", &st) == 0 && st.st_nlink == 1);

	char t[300];
	CHECK(symlink("c", "s") == 0);
	CHECK(same("s", "first"));
	CHECK(readlink("s", t, sizeof(t)) == 1 && t[0] == 'c');
	CHECK(unlink("s") == 0);
	memset(t, 'x', 120);
	t[120] = 0;
	CHECK(symlink(t, "l") == 0);     /* too long for the inode */
	char u[300];
	CHECK(readlink("l", u, sizeof(u)) == 120 && memcmp(u, t, 120) == 0);
	CHECK(unlink("l") == 0);

	int fd = open("c", O_RDONLY);
	CHECK(fd >= 0);
	CHECK(unlink("c") == 0);
	char b[8] = { 0 };
	CHECK(read(fd, b, 5) == 5 && memcmp(b, "first", 5) == 0);
	close(fd);

	CHECK(mkdir("d", 0755) == 0);
	CHECK(mkdir("d", 0755) != 0 && errno == EEXIST);
	put("d/f", "x");
	CHECK(rmdir("d") != 0 && errno == ENOTEMPTY);
	CHECK(mkdir("e", 0755) == 0);
	CHECK(rename("d", "e/g") == 0);          /* moves .. as well */
	CHECK(same("e/g/../g/f", "x"));
	CHECK(stat("e", &st) == 0 && st.st_nlink == 3);
	CHECK(unlink("e/g/f") == 0 && rmdir("e/g") == 0 && rmdir("e") == 0);

	/* make's jobserver */
	CHECK(mkfifo("q", 0600) == 0);
	CHECK(stat("q", &st) == 0 && S_ISFIFO(st.st_mode) && (st.st_mode & 0777) == 0600);
	fd = open("q", O_RDWR);
	CHECK(fd >= 0);
	CHECK(write(fd, "+", 1) == 1 && read(fd, b, 1) == 1 && b[0] == '+');
	close(fd);
	CHECK(unlink("q") == 0);

	/* mknod takes a whole path, not a process name */
	char lng[220];
	memset(lng, 'n', 200);
	lng[200] = 0;
	CHECK(mkfifo(lng, 0600) == 0);
	CHECK(unlink(lng) == 0);
}

static void lengths(void)
{
	static char big[70000];
	struct stat st;

	memset(big, 'q', sizeof(big));
	int fd = open("t", O_CREAT | O_RDWR | O_TRUNC, 0644);
	CHECK(write(fd, big, sizeof(big)) == sizeof(big));
	CHECK(ftruncate(fd, 5000) == 0);
	CHECK(ftruncate(fd, 9000) == 0);        /* the old tail must not come back */
	char b[16];
	CHECK(pread(fd, b, 4, 4998) == 4 && !memcmp(b, "qq\0\0", 4));
	CHECK(pwrite(fd, "z", 1, 3000000) == 1); /* far past EOF, through indirect blocks */
	CHECK(pread(fd, b, 2, 2000000) == 2 && !memcmp(b, "\0\0", 2));
	CHECK(pread(fd, b, 1, 3000000) == 1 && b[0] == 'z');
	CHECK(fstat(fd, &st) == 0 && st.st_size == 3000001);
	close(fd);
	fd = open("t", O_WRONLY | O_TRUNC);
	CHECK(fd >= 0);
	CHECK(fstat(fd, &st) == 0 && st.st_size == 0);
	close(fd);
	CHECK(unlink("t") == 0);
}

static void mappings(void)
{
	size_t len = 3 * 4096;
	char *m, *o;
	int fd = open("m", O_CREAT | O_RDWR | O_TRUNC, 0644);
	CHECK(fd >= 0);
	CHECK(ftruncate(fd, len) == 0);

	/* the way lld writes its output: a hole, mapped shared, then read back */
	m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	CHECK(m != MAP_FAILED);
	memset(m, 'w', len);
	CHECK(msync(m, len, MS_SYNC) == 0);
	munmap(m, len);
	char b[4];
	CHECK(pread(fd, b, 4, 8000) == 4 && !memcmp(b, "wwww", 4));

	/* a private store stays private, and a sibling mapping keeps the file's bytes */
	m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	o = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
	CHECK(m != MAP_FAILED && o != MAP_FAILED);
	m[4096] = 'P';
	CHECK(o[4096] == 'w');
	CHECK(pread(fd, b, 1, 4096) == 1 && b[0] == 'w');
	munmap(m, len);

	/* read-only made writable must still copy: the page is the file's */
	m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
	CHECK(m != MAP_FAILED);
	CHECK(m[0] == 'w');
	CHECK(mprotect(m, len, PROT_READ | PROT_WRITE) == 0);
	m[0] = 'M';
	CHECK(o[0] == 'w');
	CHECK(pread(fd, b, 1, 0) == 1 && b[0] == 'w');
	munmap(m, len);

	/* a write is seen by a mapping made before it */
	CHECK(pwrite(fd, "n", 1, 5) == 1);
	CHECK(o[5] == 'n');
	munmap(o, len);

	/* the mapping outlives the file */
	m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	CHECK(unlink("m") == 0);
	CHECK(m != MAP_FAILED && m[100] == 'w');
	munmap(m, len);
}

/* Each child owns its files; a shared allocator bug shows up as foreign bytes. */
static void together(void)
{
	enum { KIDS = 4, FILES = 20, SIZE = 20000 };
	char n[32];
	static char buf[SIZE], got[SIZE];

	CHECK(mkdir("p", 0755) == 0);
	for (int k = 0; k < KIDS; k++) {
		pid_t pid = fork();
		if (pid == 0) {
			memset(buf, 'A' + k, SIZE);
			for (int i = 0; i < FILES; i++) {
				snprintf(n, sizeof(n), "p/%d-%d.tmp", k, i);
				int fd = open(n, O_CREAT | O_WRONLY | O_EXCL, 0644);
				if (fd < 0 || write(fd, buf, SIZE) != SIZE) _exit(1);
				close(fd);
				char f[32];
				snprintf(f, sizeof(f), "p/%d-%d", k, i);
				if (rename(n, f) != 0) _exit(2);
			}
			_exit(0);
		}
		CHECK(pid > 0);
	}
	for (int k = 0; k < KIDS; k++) {
		int st = 0;
		CHECK(wait(&st) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
	}
	for (int k = 0; k < KIDS; k++)
		for (int i = 0; i < FILES; i++) {
			snprintf(n, sizeof(n), "p/%d-%d", k, i);
			int fd = open(n, O_RDONLY);
			CHECK(fd >= 0 && read(fd, got, SIZE) == SIZE);
			close(fd);
			memset(buf, 'A' + k, SIZE);
			CHECK(memcmp(got, buf, SIZE) == 0);
			CHECK(unlink(n) == 0);
		}
	CHECK(rmdir("p") == 0);
}

/* execve resolves a relative name against the cwd, as make's ./tool does */
static void relative_exec(void)
{
	pid_t pid = fork();
	if (pid == 0) {
		if (chdir("/bin") != 0) _exit(2);
		execl("./busybox", "busybox", "true", (char *)NULL);
		_exit(3);
	}
	int st = 0;
	CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* Loading a program from disk can sleep on the filesystem. The new entry point
 * must still reach the process: several at once make that sleep likely. */
static void execs_while_contended(void)
{
	static char buf[65536];
	int in = open("/bin/busybox", O_RDONLY);
	int out = open("bb", O_CREAT | O_WRONLY | O_TRUNC, 0755);
	CHECK(in >= 0 && out >= 0);
	ssize_t n;
	while ((n = read(in, buf, sizeof(buf))) > 0) CHECK(write(out, buf, n) == n);
	close(in);
	close(out);
	for (int k = 0; k < 4; k++) {
		pid_t pid = fork();
		if (pid == 0) {
			for (int i = 0; i < 25; i++) {
				pid_t c = fork();
				if (c == 0) { execl("./bb", "true", (char *)NULL); _exit(9); }
				int st = 0;
				if (waitpid(c, &st, 0) != c || !WIFEXITED(st) || WEXITSTATUS(st)) _exit(1);
			}
			_exit(0);
		}
		CHECK(pid > 0);
	}
	for (int k = 0; k < 4; k++) {
		int st = 0;
		CHECK(wait(&st) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
	}
	CHECK(unlink("bb") == 0);
}

int main(int argc, char **argv)
{
	relative_exec();
	if (argc < 2 || chdir(argv[1]) != 0) { puts("FAIL usage"); return 1; }
	CHECK(mkdir("ext2rw", 0755) == 0);
	CHECK(chdir("ext2rw") == 0);
	names();
	lengths();
	/* file mappings need the block to be a whole page */
	struct statvfs sv;
	if (statvfs(".", &sv) == 0 && sv.f_bsize == 4096) mappings();
	together();
	execs_while_contended();
	CHECK(chdir("..") == 0);
	CHECK(rmdir("ext2rw") == 0);
	if (fails) printf("FAIL line %d errno=%d, %d checks\n", first_bad, first_errno, fails);
	else puts("OK");
	return fails != 0;
}
