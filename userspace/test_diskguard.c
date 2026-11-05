/* A protected block device takes no write by any route. usage: test_diskguard /dev/X
 * Prints one line per route; the caller also compares the image's hash from outside. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/uio.h>
#include <unistd.h>

#define BLKROSET      0x125D
#define BLKROGET      0x125E
#define BLKRANIXALLOW 0x12F0

static int fails;

static void expect(const char *what, long got, int want_errno)
{
	int ok = got < 0 && errno == want_errno;
	printf("%s: %s (ret %ld errno %d, want %d)\n", ok ? "PASS" : "FAIL", what, got, errno, want_errno);
	if (!ok) fails++;
}

int main(int argc, char **argv)
{
	static char zero[4096], before[4096], after[4096];
	if (argc < 2) return 2;
	int fd = open(argv[1], O_RDWR);
	if (fd < 0) { printf("FAIL: open %s %d\n", argv[1], errno); return 1; }
	int ro = -1;
	ioctl(fd, BLKROGET, &ro);
	printf("%s: BLKROGET=%d\n", ro == 1 ? "PASS" : "FAIL", ro);
	if (ro != 1) fails++;
	if (pread(fd, before, sizeof(before), 0) != sizeof(before)) { printf("FAIL: read\n"); fails++; }

	errno = 0; expect("write", write(fd, zero, 512), EROFS);
	errno = 0; expect("pwrite", pwrite(fd, zero, 4096, 0), EROFS);
	errno = 0; expect("pwrite past 1M", pwrite(fd, zero, 512, 1 << 20), EROFS);
	struct iovec iov = { zero, 512 };
	errno = 0; expect("writev", writev(fd, &iov, 1), EROFS);
	errno = 0;
	long pv = pwritev(fd, &iov, 1, 512);
	if (pv < 0 && errno == ENOSYS) printf("PASS: pwritev does not exist here\n");
	else expect("pwritev", pv, EROFS);
	int z = open("/dev/zero", O_RDONLY);
	errno = 0; expect("sendfile into it", sendfile(fd, z, NULL, 512), EROFS);
	close(z);

	void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m != MAP_FAILED) {
		/* no device mmap exists, so this can only be private memory: stores must stay there */
		memset(m, 0xA5, 4096);
		msync(m, 4096, MS_SYNC);
		munmap(m, 4096);
	}
	printf("PASS: shared mapping tried (%s)\n", m == MAP_FAILED ? "refused" : "stores stayed in memory if sector 0 is unchanged below");

	int off = 0;
	errno = 0; expect("BLKROSET 0 cannot lift it", ioctl(fd, BLKROSET, &off), EPERM);
	errno = 0; expect("allow with a wrong identity", ioctl(fd, BLKRANIXALLOW, "serial:not-this-disk"), EPERM);
	errno = 0; expect("allow with an empty identity", ioctl(fd, BLKRANIXALLOW, ""), EPERM);
	ro = -1;
	ioctl(fd, BLKROGET, &ro);
	printf("%s: still read-only (%d)\n", ro == 1 ? "PASS" : "FAIL", ro);
	if (ro != 1) fails++;

	if (pread(fd, after, sizeof(after), 0) != sizeof(after) || memcmp(before, after, sizeof(after))) {
		printf("FAIL: sector 0 changed\n");
		fails++;
	} else {
		printf("PASS: sector 0 unchanged\n");
	}
	close(fd);
	printf("diskguard %s fails=%d\n", fails ? "FAILED" : "OK", fails);
	return fails != 0;
}
