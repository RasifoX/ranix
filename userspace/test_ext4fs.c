/* ext2/4 statfs: the on-disk superblock geometry is reported (block size,
 * total/free block and inode counts), not the ramfs nominal. Runs only where a
 * real disk is mounted at /mnt. First failure or OK. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/vfs.h>
#include <sys/stat.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s (errno=%d)\n", __LINE__, #c, errno); return 0; } } while (0)

int main(void)
{
	struct statfs s;
	CHECK(statfs("/mnt", &s) == 0);

	/* ext2/ext4 magic */
	CHECK((unsigned long)s.f_type == 0xEF53UL);
	/* the test image is mkfs.ext4 -b 1024 */
	CHECK(s.f_bsize == 1024);
	/* 32 MB / 1 KiB = 32768 blocks total */
	CHECK(s.f_blocks == 32768);
	/* free < total and a sane chunk is used by metadata + seeded files */
	CHECK(s.f_bfree > 0 && s.f_bfree < s.f_blocks);
	/* inode table present */
	CHECK(s.f_files == 8192);
	CHECK(s.f_ffree > 0 && s.f_ffree < s.f_files);

	puts("OK");
	return 0;
}
