/* Reproduce the Python-zip memory corruption: a large resident ramfs file
 * (forces an order-12 buddy block) kept warm, plus heavy concurrent create /
 * grow-write / stat / lookup / unlink churn of many small files in the same
 * directory tree. If a buddy/slab page aliases the big file's buffer, the
 * directory entry list gets clobbered with file bytes and lookup GP-faults. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); return 0; } } while (0)

#define BIG_SZ   (9u * 1024 * 1024 + 400 * 1024)   /* ~9.4MB → order-12 */

static char buf[64 * 1024];

int main(void)
{
	mkdir("/tmp/churn", 0755);

	/* 1. lay down the big resident file (single large write → order-12) */
	memset(buf, 'T', sizeof(buf));
	int bf = open("/tmp/churn/big.zip", O_CREAT | O_WRONLY | O_TRUNC, 0644);
	CHECK(bf >= 0);
	unsigned written = 0;
	while (written < BIG_SZ) {
		unsigned n = BIG_SZ - written;
		if (n > sizeof(buf)) n = sizeof(buf);
		CHECK(write(bf, buf, n) == (ssize_t)n);
		written += n;
	}
	close(bf);

	/* 2. churn: create many small files that grow across order boundaries,
	 *    stat/read the big file repeatedly, then unlink. Mirrors Python's
	 *    __pycache__ writes + stdlib zip stat/read storm. */
	for (int round = 0; round < 40; round++) {
		for (int i = 0; i < 64; i++) {
			char path[64];
			snprintf(path, sizeof(path), "/tmp/churn/f%d_%d.pyc", round, i);
			int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
			CHECK(fd >= 0);
			/* grow in chunks so ramfs_write hits its buddy grow path */
			for (int k = 0; k < 6; k++)
				CHECK(write(fd, buf, 2048 + k * 1500) > 0);
			close(fd);

			/* stat the big file — this walks the dir entry list (lookup) */
			struct stat st;
			CHECK(stat("/tmp/churn/big.zip", &st) == 0);
			CHECK(st.st_size == BIG_SZ);

			/* read a slice of the big file to keep its buffer hot */
			int rb = open("/tmp/churn/big.zip", O_RDONLY);
			CHECK(rb >= 0);
			CHECK(read(rb, buf, 4096) == 4096);
			close(rb);
		}
		/* sweep-unlink this round's files */
		for (int i = 0; i < 64; i++) {
			char path[64];
			snprintf(path, sizeof(path), "/tmp/churn/f%d_%d.pyc", round, i);
			unlink(path);
		}
		memset(buf, 'T', sizeof(buf));
	}

	/* 3. final sanity: big file still intact and lookable */
	struct stat st;
	CHECK(stat("/tmp/churn/big.zip", &st) == 0 && st.st_size == BIG_SZ);
	unlink("/tmp/churn/big.zip");
	rmdir("/tmp/churn");

	puts("OK");
	return 0;
}
