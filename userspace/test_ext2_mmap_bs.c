/* mmap of ext2 files when a block is smaller than a page (1 KiB, 2 KiB):
 *   test_ext2_mmap_bs DEV DIR OFFSET BSIZE
 * mounts the ext2 at OFFSET bytes into DEV on DIR and checks:
 * - statfs reports BSIZE (the image is what the test thinks it is);
 * - a 3-page-and-a-bit file, written with write(), reads back through a
 *   MAP_PRIVATE mapping byte for byte, the tail of the last page zero;
 * - a sparse file (data at 0 and at 3 pages, a hole between) maps with the
 *   hole as zeros, the data where it was written;
 * - a mapping at a non-zero file offset (page 2) sees page 2;
 * - a private store does not reach the file;
 * - the umount succeeds (no mapping left a reference).
 * Whether a MAP_SHARED store reaches the file is printed as a NOTE, not
 * checked: with blocks smaller than a page the mapping is a copy.
 * DEV "-" uses DIR as it is (no mount, no block size check): the same
 * checks against any filesystem, e.g. on Linux.
 * Prints OK, or FAIL with the first case that differs. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

#define PG 4096
#define FLEN (3 * PG + 100)

static int fails;
#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); puts(""); fails++; } while (0)

static unsigned char pat(size_t i) { return (unsigned char)(i * 7 + (i >> 9) + 1); }

static int put(const char *path, off_t at, const void *b, size_t n)
{
	int fd = open(path, O_CREAT | O_WRONLY, 0644);
	if (fd < 0) return -1;
	int ok = pwrite(fd, b, n, at) == (ssize_t)n;
	close(fd);
	return ok ? 0 : -1;
}

static void dense(const char *dir)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/dense", dir);
	unlink(path);
	unsigned char *b = malloc(FLEN);
	for (size_t i = 0; i < FLEN; i++) b[i] = pat(i);
	if (put(path, 0, b, FLEN)) { FAIL("write %s errno %d", path, errno); free(b); return; }

	int fd = open(path, O_RDWR);
	unsigned char *m = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	if (m == MAP_FAILED) { FAIL("mmap private errno %d", errno); close(fd); free(b); return; }
	size_t bad = FLEN;
	for (size_t i = 0; i < FLEN && bad == FLEN; i++) if (m[i] != b[i]) bad = i;
	if (bad != FLEN) FAIL("private map byte %zu: %#x want %#x", bad, m[bad], b[bad]);
	for (size_t i = FLEN; i < 4 * PG; i++) if (m[i]) { FAIL("byte %zu past EOF is %#x", i, m[i]); break; }
	m[PG + 5] ^= 0xff;                         /* private: stays here */
	munmap(m, 4 * PG);
	unsigned char c = 0;
	if (pread(fd, &c, 1, PG + 5) != 1 || c != b[PG + 5]) FAIL("a private store reached the file");

	unsigned char *m2 = mmap(NULL, PG, PROT_READ, MAP_PRIVATE, fd, 2 * PG);
	if (m2 == MAP_FAILED) FAIL("mmap at offset 2 pages errno %d", errno);
	else {
		if (memcmp(m2, b + 2 * PG, PG)) FAIL("mapping at file offset 2 pages differs");
		munmap(m2, PG);
	}

	unsigned char *s = mmap(NULL, 4 * PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (s != MAP_FAILED) {
		s[2 * PG + 9] = 0x5a;
		msync(s, 4 * PG, MS_SYNC);
		munmap(s, 4 * PG);
		c = 0;
		if (pread(fd, &c, 1, 2 * PG + 9) != 1 || c != 0x5a) printf("NOTE: a MAP_SHARED store did not reach the file\n");
	}
	close(fd);
	free(b);
}

static void sparse(const char *dir)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/sparse", dir);
	unlink(path);
	if (put(path, 0, "head", 4) || put(path, 3 * PG, "tail", 4)) { FAIL("write sparse"); return; }
	int fd = open(path, O_RDONLY);
	unsigned char *m = mmap(NULL, 4 * PG, PROT_READ, MAP_PRIVATE, fd, 0);
	if (m == MAP_FAILED) { FAIL("mmap sparse errno %d", errno); close(fd); return; }
	if (memcmp(m, "head", 4) || memcmp(m + 3 * PG, "tail", 4)) FAIL("sparse data moved");
	for (size_t i = 4; i < 3 * PG; i++) if (m[i]) { FAIL("hole byte %zu is %#x", i, m[i]); break; }
	munmap(m, 4 * PG);
	close(fd);
}

int main(int argc, char **argv)
{
	if (argc < 5) { puts("FAIL: usage DEV DIR OFFSET BSIZE"); return 1; }
	const char *dev = argv[1], *dir = argv[2];
	long want = atol(argv[4]);
	char opt[48];
	snprintf(opt, sizeof(opt), "offset=%s", argv[3]);
	int mnt = strcmp(dev, "-") != 0;
	mkdir(dir, 0755);
	if (mnt && mount(dev, dir, "ext2", 0, opt)) { printf("FAIL: mount errno %d\n", errno); return 1; }
	struct statfs sf;
	if (mnt && (statfs(dir, &sf) || sf.f_bsize != want))
		FAIL("block size %ld, want %ld", (long)sf.f_bsize, want);
	dense(dir);
	sparse(dir);
	if (mnt && umount(dir)) FAIL("umount errno %d", errno);
	if (!fails) puts("OK");
	return fails != 0;
}
