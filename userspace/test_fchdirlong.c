/* fchdir() into a directory reached by a long absolute path, then getcwd()
 * and a relative create must land in that directory, as chdir() does. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

static void one(const char *base, int depth)
{
	char path[1024], cwd[1024], mk[1100];
	struct stat st;
	snprintf(path, sizeof path, "%s", base);
	mkdir(path, 0755);
	for (int i = 0; i < depth; i++) {
		size_t n = strlen(path);
		snprintf(path + n, sizeof path - n, "/d%d_abcdefghijklmnopqrstuvwxyz_0123456789_ABCDEFGHIJ", i);
		if (mkdir(path, 0755) != 0 && errno != EEXIST) { printf("FAIL mkdir len=%zu errno=%d\n", strlen(path), errno); fails++; return; }
	}
	size_t len = strlen(path);
	if (chdir("/") != 0 || chdir(path) != 0 || !getcwd(cwd, sizeof cwd) || strcmp(cwd, path) != 0) {
		printf("FAIL chdir control len=%zu errno=%d\n", len, errno); fails++; return;
	}
	(void)!chdir("/");
	int fd = open(path, O_RDONLY | O_DIRECTORY);
	if (fd < 0) { printf("FAIL open len=%zu errno=%d\n", len, errno); fails++; return; }
	if (fchdir(fd) != 0) { printf("FAIL fchdir len=%zu errno=%d\n", len, errno); fails++; close(fd); return; }
	close(fd);
	if (!getcwd(cwd, sizeof cwd)) { printf("FAIL getcwd len=%zu errno=%d\n", len, errno); fails++; return; }
	if (strcmp(cwd, path) != 0) { printf("FAIL getcwd len=%zu got len=%zu\n", len, strlen(cwd)); fails++; return; }
	int m = open("marker", O_CREAT | O_WRONLY, 0644);
	if (m >= 0) close(m);
	snprintf(mk, sizeof mk, "%s/marker", path);
	if (m < 0 || stat(mk, &st) != 0) { printf("FAIL marker len=%zu errno=%d\n", len, errno); fails++; }
	unlink(mk);
	(void)!chdir("/");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	mkdir("/tmp", 01777);
	one("/tmp/fcl", 1);    /* short path */
	one("/tmp/fcl", 4);    /* ~230 bytes */
	one("/tmp/fcl", 6);    /* ~340 bytes */
	one("/tmp/fcl", 12);   /* ~680 bytes */
	if (!fails) printf("OK\n");
	return fails ? 1 : 0;
}
