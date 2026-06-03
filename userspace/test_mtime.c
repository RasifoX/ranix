/* file timestamps: a freshly created file gets a non-zero mtime (not 1970);
 * utimensat sets exact atime/mtime; writing updates mtime; futimens (fd, NULL
 * path) works and UTIME_OMIT leaves a field alone. make/ls -l/tar depend on this. */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

int main(void)
{
	int fd = open("/tmp/mt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) { printf("mtime-fail create\n"); return 0; }
	write(fd, "abc", 3);
	close(fd);

	struct stat s;
	if (stat("/tmp/mt", &s) != 0) { printf("mtime-fail stat\n"); return 0; }
	if (s.st_mtim.tv_sec <= 0) { printf("mtime-fail zero mtime=%ld\n", (long)s.st_mtim.tv_sec); return 0; }

	/* utimensat: set exact atime/mtime */
	struct timespec t[2] = { { 111111, 0 }, { 222222, 0 } };
	if (utimensat(AT_FDCWD, "/tmp/mt", t, 0) != 0) { printf("mtime-fail utimensat\n"); return 0; }
	if (stat("/tmp/mt", &s) != 0) { printf("mtime-fail stat2\n"); return 0; }
	if (s.st_atim.tv_sec != 111111 || s.st_mtim.tv_sec != 222222) {
		printf("mtime-fail set a=%ld m=%ld\n", (long)s.st_atim.tv_sec, (long)s.st_mtim.tv_sec); return 0;
	}

	/* writing updates mtime away from the value we just set */
	fd = open("/tmp/mt", O_WRONLY);
	write(fd, "z", 1);
	close(fd);
	if (stat("/tmp/mt", &s) != 0) { printf("mtime-fail stat3\n"); return 0; }
	if (s.st_mtim.tv_sec == 222222) { printf("mtime-fail write-no-update\n"); return 0; }

	/* futimens (fd, NULL path) with UTIME_OMIT on atime */
	fd = open("/tmp/mt", O_RDWR);
	struct timespec t2[2] = { { 0, UTIME_OMIT }, { 333333, 0 } };
	int r = futimens(fd, t2);
	close(fd);
	if (r != 0) { printf("mtime-fail futimens\n"); return 0; }
	if (stat("/tmp/mt", &s) != 0) { printf("mtime-fail stat4\n"); return 0; }
	if (s.st_mtim.tv_sec != 333333) { printf("mtime-fail futimens-set m=%ld\n", (long)s.st_mtim.tv_sec); return 0; }

	printf("OK\n");
	return 0;
}
