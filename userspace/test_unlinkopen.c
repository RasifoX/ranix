/* open-unlinked file semantics: a file's contents must survive unlink() while
 * an fd is still open. Reading, writing, and a dup'd fd all see the live data
 * until the last close. Exercised on tmpfs (/tmp) and ramfs (/run/shm). */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

static int one(const char *path)
{
	int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd < 0) { printf("fail open %s\n", path); return 1; }

	if (write(fd, "hello", 5) != 5) { printf("fail write %s\n", path); return 1; }

	if (unlink(path) != 0) { printf("fail unlink %s\n", path); return 1; }
	/* name is gone immediately */
	if (access(path, F_OK) == 0) { printf("fail name-lives %s\n", path); return 1; }

	/* read-after-unlink through the open fd: contents survive */
	char buf[16] = {0};
	if (lseek(fd, 0, SEEK_SET) != 0) { printf("fail seek %s\n", path); return 1; }
	if (read(fd, buf, 5) != 5 || memcmp(buf, "hello", 5) != 0) {
		printf("fail read-after-unlink %s got '%s'\n", path, buf); return 1;
	}

	/* write-after-unlink, then read it back */
	if (lseek(fd, 0, SEEK_SET) != 0) { printf("fail seek2 %s\n", path); return 1; }
	if (write(fd, "WORLD", 5) != 5) { printf("fail write-after-unlink %s\n", path); return 1; }

	/* a dup'd fd shares the file description and sees the same data */
	int fd2 = dup(fd);
	if (fd2 < 0) { printf("fail dup %s\n", path); return 1; }
	memset(buf, 0, sizeof(buf));
	if (lseek(fd2, 0, SEEK_SET) != 0) { printf("fail seek3 %s\n", path); return 1; }
	if (read(fd2, buf, 5) != 5 || memcmp(buf, "WORLD", 5) != 0) {
		printf("fail dup-read %s got '%s'\n", path, buf); return 1;
	}

	/* closing one fd of a dup pair keeps the data alive on the other */
	close(fd2);
	memset(buf, 0, sizeof(buf));
	if (lseek(fd, 0, SEEK_SET) != 0) { printf("fail seek4 %s\n", path); return 1; }
	if (read(fd, buf, 5) != 5 || memcmp(buf, "WORLD", 5) != 0) {
		printf("fail post-dup-close %s got '%s'\n", path, buf); return 1;
	}

	close(fd);
	return 0;
}

int main(void)
{
	if (one("/tmp/unlinkopen.tmp"))    return 0;
	if (one("/run/shm/unlinkopen.tmp")) return 0;
	printf("OK\n");
	return 0;
}
