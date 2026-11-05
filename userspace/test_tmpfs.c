/* tmpfs: size= mount option, ENOSPC at the limit, and statfs free/used
 * accounting that tracks writes and frees. First failure or OK. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s (errno=%d)\n", __LINE__, #c, errno); return 0; } } while (0)

int main(void)
{
	mkdir("/tmp/tfs", 0755);
	/* a 256 KiB tmpfs */
	CHECK(mount("tmpfs", "/tmp/tfs", "tmpfs", 0, "size=256k") == 0);

	struct statfs sf;
	CHECK(statfs("/tmp/tfs", &sf) == 0);
	CHECK(sf.f_bsize == 4096);
	CHECK((unsigned long)sf.f_type == 0x01021994UL);
	CHECK(sf.f_blocks == 256UL * 1024 / 4096);          /* 64 blocks */
	unsigned long free0 = sf.f_bfree;
	CHECK(free0 == sf.f_blocks);                          /* empty: all free */

	/* write 64 KiB; free should drop by ~16 blocks */
	int fd = open("/tmp/tfs/a", O_CREAT | O_WRONLY, 0644);
	CHECK(fd >= 0);
	char buf[4096]; memset(buf, 'x', sizeof(buf));
	for (int i = 0; i < 16; i++) CHECK(write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf));
	close(fd);
	CHECK(statfs("/tmp/tfs", &sf) == 0);
	CHECK(sf.f_bfree == free0 - 16);

	/* fill past the limit: a write that would exceed 256 KiB fails with ENOSPC */
	fd = open("/tmp/tfs/b", O_CREAT | O_WRONLY, 0644);
	CHECK(fd >= 0);
	int wrote = 0, hit_enospc = 0;
	for (int i = 0; i < 100; i++) {
		ssize_t n = write(fd, buf, sizeof(buf));
		if (n < 0) { hit_enospc = (errno == ENOSPC); break; }
		wrote += (int)n;
	}
	close(fd);
	CHECK(hit_enospc);
	CHECK(wrote <= 256 * 1024);                           /* never over-committed */

	CHECK(statfs("/tmp/tfs", &sf) == 0);
	unsigned long free_full = sf.f_bfree;                 /* mostly consumed */
	CHECK(free_full < sf.f_blocks / 2);

	/* freeing a file returns space */
	CHECK(unlink("/tmp/tfs/b") == 0);
	CHECK(statfs("/tmp/tfs", &sf) == 0);
	CHECK(sf.f_bfree > free_full);

	/* read-back integrity of the surviving file */
	fd = open("/tmp/tfs/a", O_RDONLY);
	CHECK(fd >= 0);
	char rb[4096];
	CHECK(read(fd, rb, sizeof(rb)) == (ssize_t)sizeof(rb));
	CHECK(rb[0] == 'x' && rb[4095] == 'x');
	close(fd);

	/* rename across directories, over an existing name, and of a directory */
	CHECK(mkdir("/tmp/tfs/d1", 0755) == 0 && mkdir("/tmp/tfs/d2", 0755) == 0);
	close(open("/tmp/tfs/d1/f", O_CREAT | O_WRONLY, 0644));
	close(open("/tmp/tfs/d2/g", O_CREAT | O_WRONLY, 0644));
	CHECK(rename("/tmp/tfs/d1/f", "/tmp/tfs/d2/g") == 0);
	CHECK(access("/tmp/tfs/d1/f", F_OK) != 0 && access("/tmp/tfs/d2/g", F_OK) == 0);
	CHECK(rename("/tmp/tfs/d2", "/tmp/tfs/d1/d2") == 0);
	CHECK(access("/tmp/tfs/d1/d2/g", F_OK) == 0);
	CHECK(rmdir("/tmp/tfs/d1") != 0 && errno == ENOTEMPTY);
	CHECK(unlink("/tmp/tfs/d1/d2/g") == 0 && rmdir("/tmp/tfs/d1/d2") == 0 && rmdir("/tmp/tfs/d1") == 0);

	CHECK(umount("/tmp/tfs") == 0);
	puts("OK");
	return 0;
}
