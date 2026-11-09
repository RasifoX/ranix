/* overlay closes what it opened on the lower layer:
 *   test_ovl_lower_close DEV OFFSET DIR
 * mounts an ext2 (DEV at OFFSET bytes) on DIR/lower, a tmpfs on DIR/up, and
 * an overlay of the two on DIR/merged, then:
 * - opens and closes a lower-only file through the overlay (read only), and
 *   one with write intent (copied up first): both read back right;
 * - a file opened through the overlay and unlinked on the lower layer while
 *   open gives its blocks back when the overlay file is closed;
 * - after umount of the overlay, umount of the lower ext2 succeeds: an open
 *   the overlay never closed kept it EBUSY for good.
 * Prints OK, or FAIL with the first case that differs. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>

static int fails;
#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); puts(""); fails++; } while (0)

static char lower[200], up[200], merged[200];

static int put(const char *path, const char *s, size_t n)
{
	int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0) return -1;
	int ok = write(fd, s, n) == (ssize_t)n;
	close(fd);
	return ok ? 0 : -1;
}

static long free_blocks(void)
{
	struct statfs s;
	return statfs(lower, &s) ? -1 : (long)s.f_bfree;
}

static void opens(void)
{
	char p[256], b[16] = { 0 };
	snprintf(p, sizeof(p), "%s/a", merged);
	int fd = open(p, O_RDONLY);
	if (fd < 0 || read(fd, b, sizeof(b) - 1) != 5 || strcmp(b, "lower")) FAIL("read a through the overlay");
	if (fd >= 0) close(fd);

	snprintf(p, sizeof(p), "%s/b", merged);
	fd = open(p, O_RDWR);
	if (fd < 0) { FAIL("open b O_RDWR errno %d", errno); return; }
	if (pwrite(fd, "UP", 2, 0) != 2) FAIL("write b");
	memset(b, 0, sizeof(b));
	if (pread(fd, b, sizeof(b) - 1, 0) != 5 || strcmp(b, "UPwer")) FAIL("b after copy-up '%s'", b);
	close(fd);
}

/* 64 KiB on the lower layer, opened through the overlay, unlinked below */
static void unlinked_while_open(void)
{
	char lp[256], mp[256];
	snprintf(lp, sizeof(lp), "%s/big", lower);
	snprintf(mp, sizeof(mp), "%s/big", merged);
	long before = free_blocks();
	char *z = calloc(1, 65536);
	if (put(lp, z, 65536)) { FAIL("write big errno %d", errno); free(z); return; }
	free(z);
	sync();
	long with = free_blocks();
	int fd = open(mp, O_RDONLY);
	if (fd < 0) { FAIL("open big through the overlay errno %d", errno); return; }
	if (unlink(lp)) FAIL("unlink big on the lower layer errno %d", errno);
	close(fd);
	sync();
	long after = free_blocks();
	if (with >= before || after < before)
		FAIL("blocks free before %ld, with the file %ld, after close %ld", before, with, after);
}

int main(int argc, char **argv)
{
	if (argc < 4) { puts("FAIL: usage DEV OFFSET DIR"); return 1; }
	char opt[512];
	snprintf(lower, sizeof(lower), "%s/lower", argv[3]);
	snprintf(up, sizeof(up), "%s/up", argv[3]);
	snprintf(merged, sizeof(merged), "%s/merged", argv[3]);
	mkdir(argv[3], 0755); mkdir(lower, 0755); mkdir(up, 0755); mkdir(merged, 0755);
	snprintf(opt, sizeof(opt), "offset=%s", argv[2]);
	if (mount(argv[1], lower, "ext2", 0, opt)) { printf("FAIL: mount ext2 errno %d\n", errno); return 1; }
	if (mount("tmpfs", up, "tmpfs", 0, "size=1m")) { printf("FAIL: mount tmpfs errno %d\n", errno); return 1; }
	char p[256];
	snprintf(p, sizeof(p), "%s/a", lower); put(p, "lower", 5);
	snprintf(p, sizeof(p), "%s/b", lower); put(p, "lower", 5);
	snprintf(opt, sizeof(opt), "lowerdir=%s,upperdir=%s", lower, up);
	if (mount("overlay", merged, "overlay", 0, opt)) { printf("FAIL: mount overlay errno %d\n", errno); return 1; }

	opens();
	unlinked_while_open();

	if (umount(merged)) FAIL("umount overlay errno %d", errno);
	if (umount(up)) FAIL("umount tmpfs errno %d", errno);
	if (umount(lower)) FAIL("umount lower ext2 errno %d (an overlay open was never closed)", errno);
	if (!fails) puts("OK");
	return fails != 0;
}
