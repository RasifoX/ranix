/* The corners of a disk filesystem, in the directory given as argv[1]: names at
 * the limit, rename across directories, every level of the indirect tree, the
 * size limit, a disk run full. Run on a small disk, then e2fsck the image: a
 * pass here says the answers were right, fsck says the disk stayed right. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d errno=%d\n", __LINE__, errno); fails++; } } while (0)

static void put(const char *p, const char *s)
{
	int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK(write(fd, s, strlen(s)) == (ssize_t)strlen(s));
		close(fd);
	}
}

static int is(const char *p, const char *s)
{
	char b[128];
	int fd = open(p, O_RDONLY);
	if (fd < 0) return 0;
	ssize_t n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n < 0) return 0;
	b[n] = 0;
	return !strcmp(b, s);
}

static int nlink_of(const char *p)
{
	struct stat st;
	return stat(p, &st) ? -1 : (int)st.st_nlink;
}

static void names(void)
{
	char n[300];
	memset(n, 'a', 255); n[255] = 0;
	put(n, "long");
	CHECK(is(n, "long"));
	CHECK(unlink(n) == 0);
	memset(n, 'b', 256); n[256] = 0;
	errno = 0;
	CHECK(open(n, O_CREAT | O_WRONLY, 0644) < 0 && errno == ENAMETOOLONG);
	errno = 0;
	CHECK(mkdir(n, 0755) != 0 && errno == ENAMETOOLONG);
	/* a 256-byte name must not find its 0-byte truncation */
	errno = 0;
	CHECK(open(n, O_RDONLY) < 0);
}

static void rename_dirs(void)
{
	CHECK(mkdir("r1", 0755) == 0 && mkdir("r2", 0755) == 0);
	CHECK(mkdir("r1/sub", 0755) == 0 && mkdir("r1/sub/deep", 0755) == 0);
	put("r1/sub/deep/f", "x");
	CHECK(nlink_of("r1") == 3 && nlink_of("r2") == 2);

	CHECK(rename("r1/sub", "r2/sub") == 0);       /* a directory changes parent */
	CHECK(nlink_of("r1") == 2 && nlink_of("r2") == 3);
	CHECK(is("r2/sub/deep/f", "x"));
	CHECK(is("r2/sub/deep/../deep/f", "x"));
	struct stat a, b;
	CHECK(stat("r2/sub/..", &a) == 0 && stat("r2", &b) == 0 && a.st_ino == b.st_ino);

	errno = 0;
	CHECK(rename("r2", "r2/sub/deep/loop") != 0 && errno == EINVAL);   /* into itself */
	errno = 0;
	CHECK(rename("r2/sub", "r2/sub/deep/x") != 0 && errno == EINVAL);

	put("r1/a", "new"); put("r2/b", "old");
	CHECK(rename("r1/a", "r2/b") == 0);           /* over a file, across directories */
	CHECK(is("r2/b", "new") && access("r1/a", F_OK) != 0);

	CHECK(mkdir("r1/empty", 0755) == 0 && mkdir("r2/moved", 0755) == 0);
	int before = nlink_of("r1");
	CHECK(rename("r2/moved", "r1/empty") == 0);   /* over an empty directory */
	CHECK(nlink_of("r1") == before);
	put("r1/empty/in", "y");
	CHECK(mkdir("r2/moved2", 0755) == 0);
	errno = 0;
	CHECK(rename("r2/moved2", "r1/empty") != 0 && errno == ENOTEMPTY);
	errno = 0;
	CHECK(rename("r2/moved2", "r2/b") != 0 && errno == ENOTDIR);
	errno = 0;
	CHECK(rename("r2/b", "r2/moved2") != 0 && errno == EISDIR);

	CHECK(link("r2/b", "r2/c") == 0 && nlink_of("r2/b") == 2);
	CHECK(rename("r2/b", "r2/c") == 0);           /* two names of a file: nothing happens */
	CHECK(is("r2/b", "new") && is("r2/c", "new"));
}

/* One marked block at each boundary of the tree, read back, then cut level by level. */
static void indirect(uint64_t bs, int triple)
{
	uint64_t ppp = bs / 4;
	uint64_t at[] = { 0, 11, 12, 11 + ppp, 12 + ppp, 11 + ppp + ppp * ppp, 12 + ppp + ppp * ppp,
	                  12 + ppp + ppp * ppp + ppp + 1 };
	int n = triple ? 8 : 6;
	int fd = open("sparse", O_CREAT | O_RDWR | O_TRUNC, 0644);
	CHECK(fd >= 0);
	for (int i = 0; i < n; i++) {
		uint64_t v = at[i] ^ 0x5A5A;
		CHECK(pwrite(fd, &v, 8, (off_t)(at[i] * bs)) == 8);
	}
	for (int i = 0; i < n; i++) {
		uint64_t v = 0;
		CHECK(pread(fd, &v, 8, (off_t)(at[i] * bs)) == 8 && v == (at[i] ^ 0x5A5A));
		if (at[i]) {
			v = 1;
			CHECK(pread(fd, &v, 8, (off_t)(at[i] * bs - 8)) == 8 && v == 0);   /* the hole before it */
		}
	}
	CHECK(fsync(fd) == 0);
	for (int i = n - 1; i > 0; i--) {
		CHECK(ftruncate(fd, (off_t)(at[i] * bs + 4)) == 0);
		uint64_t v = 0;
		CHECK(pread(fd, &v, 8, (off_t)(at[i - 1] * bs)) == 8 && v == (at[i - 1] ^ 0x5A5A));
	}
	close(fd);
	CHECK(unlink("sparse") == 0);
}

static void limits(uint64_t bs)
{
	uint64_t ppp = bs / 4, max = (12 + ppp + ppp * ppp + ppp * ppp * ppp) * bs;
	int fd = open("big", O_CREAT | O_RDWR | O_TRUNC, 0644);
	CHECK(fd >= 0);
	char c = 'x', d = 0;
	errno = 0;
	CHECK(pwrite(fd, &c, 1, (off_t)max) < 0 && errno == EFBIG);
	errno = 0;
	CHECK(pwrite(fd, &c, 1, (off_t)max + (off_t)(bs << 32)) < 0 && errno == EFBIG);
	errno = 0;
	CHECK(ftruncate(fd, (off_t)max + 1) != 0 && errno == EFBIG);
	CHECK(pwrite(fd, &c, 1, (off_t)max - 1) == 1);  /* the very last byte is fine */
	CHECK(pread(fd, &d, 1, (off_t)max - 1) == 1 && d == 'x');
	close(fd);
	CHECK(unlink("big") == 0);
}

static uint64_t avail(void)
{
	struct statvfs s;
	return statvfs(".", &s) ? 0 : (uint64_t)s.f_bavail;
}

static void full_disk(uint64_t bs)
{
	static char b[65536];
	memset(b, 0x77, sizeof(b));
	CHECK(mkdir("fill", 0755) == 0);
	put("fill/keep", "keep-old");
	put("fill/moving", "keep-new");
	sync();
	uint64_t start = avail();

	int fd = open("fill/hog", O_CREAT | O_WRONLY, 0644);
	CHECK(fd >= 0);
	uint64_t total = 0;
	ssize_t r;
	while ((r = write(fd, b, sizeof(b))) > 0) total += (uint64_t)r;
	CHECK(r < 0 && errno == ENOSPC);
	CHECK(total > 0);
	CHECK(fsync(fd) == 0);
	close(fd);
	int i, made = 0;
	char n[64];
	/* the directory blocks left go too */
	for (i = 0; i < 200000; i++) {
		snprintf(n, sizeof(n), "fill/n%06d-a-long-enough-name-to-fill-blocks", i);
		fd = open(n, O_CREAT | O_WRONLY, 0644);
		if (fd < 0) break;
		close(fd);
		made++;
	}
	CHECK(fd < 0 && (errno == ENOSPC));
	errno = 0;
	CHECK(mkdir("fill/d1-a-long-enough-name-to-fill-more-of-the-blocks", 0755) != 0 && errno == ENOSPC);
	errno = 0;
	CHECK(symlink("/some/target/that/will/not/fit/in/a/block/because/it/is/long/enough/to/need/one/of/its/own/block",
	              "fill/sym1-a-long-enough-name-to-fill-more-of-the-blocks") != 0 && errno == ENOSPC);
	CHECK(rename("fill/moving", "fill/keep") == 0);   /* needs no room, must not lose either */
	CHECK(is("fill/keep", "keep-new"));

	CHECK(unlink("fill/hog") == 0);
	for (i = 0; i < made; i++) {
		snprintf(n, sizeof(n), "fill/n%06d-a-long-enough-name-to-fill-blocks", i);
		CHECK(unlink(n) == 0);
	}
	/* freed space is usable at once, commit or not */
	fd = open("fill/again", O_CREAT | O_WRONLY, 0644);
	CHECK(fd >= 0 && write(fd, b, sizeof(b)) == (ssize_t)sizeof(b));
	close(fd);
	CHECK(unlink("fill/again") == 0);
	sync();
	/* the directory itself grew; that is all that may remain */
	CHECK((avail() + 64) * bs >= start * bs);
	CHECK(unlink("fill/keep") == 0);
	CHECK(rmdir("fill") == 0);
}

static void big_dir(void)
{
	char n[64];
	CHECK(mkdir("many", 0755) == 0);
	for (int i = 0; i < 5000; i++) {
		snprintf(n, sizeof(n), "many/%d", i);
		int fd = open(n, O_CREAT | O_WRONLY, 0644);
		CHECK(fd >= 0);
		if (fd >= 0) close(fd);
	}
	for (int i = 0; i < 5000; i += 2) {
		snprintf(n, sizeof(n), "many/%d", i);
		CHECK(unlink(n) == 0);
	}
	DIR *d = opendir("many");
	CHECK(d != NULL);
	int cnt = 0;
	if (d) {
		while (readdir(d)) cnt++;
		closedir(d);
	}
	CHECK(cnt == 2500 + 2);
	for (int i = 1; i < 5000; i += 2) {
		snprintf(n, sizeof(n), "many/%d", i);
		CHECK(unlink(n) == 0);
	}
	CHECK(rmdir("many") == 0);
}

static void open_unlinked(void)
{
	int fd = open("gone", O_CREAT | O_RDWR, 0644);
	int fd2 = open("gone2", O_CREAT | O_RDWR, 0644);
	CHECK(fd >= 0 && fd2 >= 0);
	CHECK(unlink("gone") == 0 && unlink("gone2") == 0);
	CHECK(write(fd, "still", 5) == 5);
	sync();
	char b[8] = {0};
	CHECK(pread(fd, b, 5, 0) == 5 && !strcmp(b, "still"));
	close(fd);        /* the first of two on the orphan list */
	close(fd2);
}

int main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2 || chdir(argv[1]) != 0) { puts("FAIL usage: test_ext2hard DIR [nofill]"); return 2; }
	struct statvfs v;
	CHECK(statvfs(".", &v) == 0);
	uint64_t bs = v.f_bsize;
	names();
	rename_dirs();
	indirect(bs, 1);
	limits(bs);
	big_dir();
	open_unlinked();
	if (argc < 3) full_disk(bs);
	if (fails) { printf("FAIL %d\n", fails); return 1; }
	puts("OK");
	return 0;
}
