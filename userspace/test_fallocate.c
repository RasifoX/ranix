/* fallocate(2) actually grows the file and zero-fills; copy_file_range(2)
 * honors explicit off_in/off_out and advances them. */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>

int main(void)
{
	const char *f = "/tmp/fa_file";
	int fd = open(f, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) { printf("fallocate-fail create\n"); return 0; }
	write(fd, "hi", 2);

	if (fallocate(fd, 0, 0, 4096) != 0) { printf("fallocate-fail call errno=%d\n", errno); return 0; }

	struct stat st;
	if (fstat(fd, &st) != 0 || st.st_size != 4096) {
		printf("fallocate-fail size=%lld\n", (long long)st.st_size); return 0;
	}

	/* the region past the original 2 bytes must read back as zero */
	char buf[16];
	if (pread(fd, buf, sizeof buf, 100) != (ssize_t)sizeof buf) { printf("fallocate-fail pread\n"); return 0; }
	for (unsigned i = 0; i < sizeof buf; i++)
		if (buf[i] != 0) { printf("fallocate-fail nonzero@%u\n", i); return 0; }

	/* fallocate on a read-only fd must fail */
	int rfd = open(f, O_RDONLY);
	errno = 0;
	if (fallocate(rfd, 0, 0, 8192) != -1 || errno != EBADF) { printf("fallocate-fail rdonly errno=%d\n", errno); return 0; }
	close(rfd);
	close(fd);

	/* copy_file_range with explicit offsets */
	const char *src = "/tmp/fa_src", *dst = "/tmp/fa_dst";
	int s = open(src, O_RDWR | O_CREAT | O_TRUNC, 0644);
	write(s, "0123456789", 10);
	int d = open(dst, O_RDWR | O_CREAT | O_TRUNC, 0644);
	write(d, "..........", 10);

	off_t in_off = 4, out_off = 2;
	ssize_t n = syscall(SYS_copy_file_range, s, &in_off, d, &out_off, (size_t)3, 0u);
	if (n != 3) { printf("fallocate-fail cfr-n=%zd\n", n); return 0; }
	if (in_off != 7 || out_off != 5) { printf("fallocate-fail cfr-off %lld %lld\n", (long long)in_off, (long long)out_off); return 0; }

	char db[16];
	int got = (int)pread(d, db, 10, 0);
	if (got != 10) { printf("fallocate-fail cfr-read\n"); return 0; }
	db[10] = '\0';
	/* dst was "..........", copied "456" from src[4..7] into dst[2..5) */
	if (memcmp(db, "..456.....", 10) != 0) { printf("fallocate-fail cfr-data=%s\n", db); return 0; }
	close(s); close(d);

	printf("OK\n");
	return 0;
}
