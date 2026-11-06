/* utimensat with AT_SYMLINK_NOFOLLOW acts on the link, which may dangle: tar and
 * dpkg restore a link's time before its target exists. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static int on(const char *dir)
{
	char l[128], f[128], t[128];
	snprintf(l, sizeof l, "%s/.un_l", dir);
	snprintf(f, sizeof f, "%s/.un_f", dir);
	snprintf(t, sizeof t, "%s/.un_none", dir);
	unlink(l); unlink(f);
	struct timespec ts[2] = {{1000000, 0}, {2000000, 0}};
	struct stat st;
	if (symlink(t, l)) { puts("FAIL symlink"); return 1; }
	if (utimensat(AT_FDCWD, l, ts, AT_SYMLINK_NOFOLLOW)) { perror("FAIL dangling"); return 1; }
	if (lstat(l, &st) || st.st_mtime != 2000000) { printf("FAIL mtime %ld\n", (long)st.st_mtime); return 1; }
	if (utimensat(AT_FDCWD, l, ts, 0) == 0) { puts("FAIL followed a dangling link"); return 1; }
	unlink(l);
	int fd = open(f, O_CREAT | O_WRONLY, 0644);
	if (fd < 0) { puts("FAIL open"); return 1; }
	close(fd);
	if (symlink(f, l)) { puts("FAIL symlink2"); return 1; }
	if (utimensat(AT_FDCWD, l, ts, AT_SYMLINK_NOFOLLOW)) { perror("FAIL live"); return 1; }
	if (stat(f, &st) || st.st_mtime == 2000000) { puts("FAIL target touched"); return 1; }
	unlink(l); unlink(f);
	return 0;
}

int main(int argc, char **argv)
{
	if (on(argc > 1 ? argv[1] : "/tmp")) return 1;
	puts("OK");
	return 0;
}
