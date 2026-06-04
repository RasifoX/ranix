/* overlayfs: read-through union of lower+upper, copy-up on write, and a
 * whiteout that hides a lower file after delete. First failure or OK. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <dirent.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %d %s (errno=%d)\n", __LINE__, #c, errno); return 0; } } while (0)

static int read_file(const char *p, char *buf, int sz)
{
	int fd = open(p, O_RDONLY);
	if (fd < 0) return -1;
	int n = (int)read(fd, buf, sz - 1);
	close(fd);
	if (n < 0) return -1;
	buf[n] = '\0';
	return n;
}

static int write_file(const char *p, const char *s)
{
	int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) return -1;
	int n = (int)write(fd, s, strlen(s));
	close(fd);
	return n == (int)strlen(s) ? 0 : -1;
}

int main(void)
{
	/* tmpfs scratch so the layers are writable and isolated */
	mkdir("/tmp/ovl", 0755);
	CHECK(mount("tmpfs", "/tmp/ovl", "tmpfs", 0, "size=4m") == 0);
	mkdir("/tmp/ovl/lower", 0755);
	mkdir("/tmp/ovl/upper", 0755);
	mkdir("/tmp/ovl/work",  0755);
	mkdir("/tmp/ovl/merged", 0755);

	/* seed the lower (read-only) layer */
	CHECK(write_file("/tmp/ovl/lower/base.txt", "from-lower") == 0);
	CHECK(write_file("/tmp/ovl/lower/keep.txt", "keepme") == 0);

	CHECK(mount("overlay", "/tmp/ovl/merged", "overlay", 0,
	            "lowerdir=/tmp/ovl/lower,upperdir=/tmp/ovl/upper,workdir=/tmp/ovl/work") == 0);

	char buf[128];

	/* read-through: lower file visible in the merged view */
	CHECK(read_file("/tmp/ovl/merged/base.txt", buf, sizeof(buf)) > 0);
	CHECK(strcmp(buf, "from-lower") == 0);

	/* copy-up: writing the lower file lands in upper, lower stays intact */
	CHECK(write_file("/tmp/ovl/merged/base.txt", "from-upper") == 0);
	CHECK(read_file("/tmp/ovl/merged/base.txt", buf, sizeof(buf)) > 0);
	CHECK(strcmp(buf, "from-upper") == 0);
	CHECK(read_file("/tmp/ovl/lower/base.txt", buf, sizeof(buf)) > 0);
	CHECK(strcmp(buf, "from-lower") == 0);                 /* lower untouched */
	CHECK(read_file("/tmp/ovl/upper/base.txt", buf, sizeof(buf)) > 0);
	CHECK(strcmp(buf, "from-upper") == 0);                 /* upper got the copy */

	/* new file created through merged goes to upper only */
	CHECK(write_file("/tmp/ovl/merged/new.txt", "brand-new") == 0);
	CHECK(read_file("/tmp/ovl/upper/new.txt", buf, sizeof(buf)) > 0);
	CHECK(strcmp(buf, "brand-new") == 0);
	CHECK(access("/tmp/ovl/lower/new.txt", F_OK) != 0);    /* not in lower */

	/* whiteout: deleting a lower-only file hides it from the merged view */
	CHECK(unlink("/tmp/ovl/merged/keep.txt") == 0);
	CHECK(access("/tmp/ovl/merged/keep.txt", F_OK) != 0);  /* gone from merge */
	CHECK(read_file("/tmp/ovl/lower/keep.txt", buf, sizeof(buf)) > 0); /* lower kept */

	/* readdir merge: base.txt (copied up) + new.txt (upper) present,
	 * keep.txt hidden by its whiteout */
	{
		DIR *d = opendir("/tmp/ovl/merged");
		CHECK(d != NULL);
		int seen_base = 0, seen_new = 0, seen_keep = 0, seen_wh = 0;
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			if (!strcmp(de->d_name, "base.txt")) seen_base = 1;
			else if (!strcmp(de->d_name, "new.txt")) seen_new = 1;
			else if (!strcmp(de->d_name, "keep.txt")) seen_keep = 1;
			else if (de->d_name[0] != '.') seen_wh = 1;  /* no stray entries */
		}
		closedir(d);
		CHECK(seen_base && seen_new);
		CHECK(!seen_keep);                                 /* whiteout hides it */
		CHECK(!seen_wh);
	}

	CHECK(umount("/tmp/ovl/merged") == 0);
	puts("OK");
	return 0;
}
