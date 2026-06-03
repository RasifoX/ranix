/* mknod(2) device nodes: creating a char node with a major:minor routes to the
 * matching driver (devfs registry), so a hand-made /dev/null behaves like the
 * real one. This is what minimal-rootfs setup (debootstrap-style) does. (root) */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

int main(void)
{
	unlink("/tmp/mynull");
	if (mknod("/tmp/mynull", S_IFCHR | 0666, makedev(1, 3)) != 0) { printf("mknoddev-fail null\n"); return 0; }

	struct stat s;
	if (stat("/tmp/mynull", &s) != 0 || !S_ISCHR(s.st_mode) || s.st_rdev != makedev(1, 3)) {
		printf("mknoddev-fail stat rdev=%lx\n", (unsigned long)s.st_rdev); return 0;
	}

	int fd = open("/tmp/mynull", O_RDWR);
	if (fd < 0) { printf("mknoddev-fail open\n"); return 0; }
	if (write(fd, "abc", 3) != 3) { printf("mknoddev-fail null-write\n"); return 0; }  /* discarded */
	char b[8];
	if (read(fd, b, sizeof(b)) != 0) { printf("mknoddev-fail null-read\n"); return 0; }  /* EOF */
	close(fd);

	/* a /dev/zero clone (1:5) yields zero bytes */
	unlink("/tmp/myzero");
	if (mknod("/tmp/myzero", S_IFCHR | 0666, makedev(1, 5)) != 0) { printf("mknoddev-fail zero\n"); return 0; }
	fd = open("/tmp/myzero", O_RDONLY);
	memset(b, 0xAA, sizeof(b));
	if (read(fd, b, 4) != 4 || b[0] != 0 || b[3] != 0) { printf("mknoddev-fail zero-read\n"); return 0; }
	close(fd);

	printf("OK\n");
	return 0;
}
