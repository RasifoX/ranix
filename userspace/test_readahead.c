/* Sequential reads coalesce into far fewer device transfers than sectors moved,
 * proving the block-layer readahead prefetches contiguous runs. Reads the
 * seeded /mnt/seq.dat while watching the parent disk's /sys/block/<dev>/stat
 * counters. Runs only where a real disk is mounted at /mnt. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s\n", __LINE__, #c); return 0; } } while (0)

static int read_stat(const char *dev, unsigned long long *reads, unsigned long long *secs)
{
	char path[128];
	snprintf(path, sizeof(path), "/sys/block/%s/stat", dev);
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	char buf[64] = {0};
	int n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) return -1;
	return sscanf(buf, "%llu %llu", reads, secs) == 2 ? 0 : -1;
}

int main(void)
{
	/* whole-disk node carries the counters; partitions forward to it. pick the
	 * first /sys/block entry without a 'p<n>' partition suffix. */
	char dev[32] = {0};
	DIR *d = opendir("/sys/block");
	CHECK(d != NULL);
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.') continue;
		size_t l = strlen(e->d_name);
		if (l >= 2 && e->d_name[l - 2] == 'p' && e->d_name[l - 1] >= '0' &&
		    e->d_name[l - 1] <= '9') continue;
		strncpy(dev, e->d_name, sizeof(dev) - 1);
		break;
	}
	closedir(d);
	CHECK(dev[0] != 0);

	unsigned long long r0, s0, r1, s1;
	CHECK(read_stat(dev, &r0, &s0) == 0);

	int fd = open("/mnt/seq.dat", O_RDONLY);
	CHECK(fd >= 0);
	char buf[4096];
	long long total = 0, n;
	while ((n = read(fd, buf, sizeof(buf))) > 0) total += n;
	close(fd);
	CHECK(total == 262144);

	CHECK(read_stat(dev, &r1, &s1) == 0);
	unsigned long long dr = r1 - r0, ds = s1 - s0;

	/* the file is 256 KiB; at 1 KiB blocks that's 256 blocks / 512 sectors. With
	 * readahead the run is fetched in a handful of multi-block transfers, so
	 * sectors moved must outnumber transfers by a wide margin. */
	CHECK(ds >= 256);
	CHECK(dr > 0 && dr * 4 < ds);

	puts("OK");
	return 0;
}
