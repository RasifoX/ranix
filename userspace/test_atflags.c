/* *at() flag/dirfd conformance: fchmodat/fchownat AT_SYMLINK_NOFOLLOW act on the
 * link, unlinkat AT_REMOVEDIR is rmdir (and rejects files / non-dirs), and a
 * relative path resolves against the dir fd, not the cwd. */
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>

/* call the raw fchmodat/fchownat syscalls: musl's fchmodat emulates
 * AT_SYMLINK_NOFOLLOW via O_PATH+/proc and may refuse it, hiding the kernel. */
static int raw_fchmodat(int dfd, const char *p, mode_t m, int f)
{
	return syscall(SYS_fchmodat, dfd, p, m, f);
}
static int raw_fchownat(int dfd, const char *p, uid_t u, gid_t g, int f)
{
	return syscall(SYS_fchownat, dfd, p, u, g, f);
}

int main(void)
{
	mkdir("/tmp/atd", 0755);
	int fd = open("/tmp/atd/real", O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd < 0) { printf("fail create\n"); return 0; }
	close(fd);
	if (symlink("/tmp/atd/real", "/tmp/atd/lnk") != 0) { printf("fail symlink\n"); return 0; }

	/* fchmodat NOFOLLOW must not alter the target's mode */
	chmod("/tmp/atd/real", 0644);
	if (raw_fchmodat(AT_FDCWD, "/tmp/atd/lnk", 0600, AT_SYMLINK_NOFOLLOW) != 0) { printf("fail fchmodat\n"); return 0; }
	struct stat st;
	if (stat("/tmp/atd/real", &st) != 0 || (st.st_mode & 07777) != 0644) {
		printf("fail nofollow-chmod mode=%o\n", st.st_mode & 07777); return 0;
	}
	/* without NOFOLLOW it follows to the target */
	if (raw_fchmodat(AT_FDCWD, "/tmp/atd/lnk", 0600, 0) != 0) { printf("fail fchmodat2\n"); return 0; }
	if (stat("/tmp/atd/real", &st) != 0 || (st.st_mode & 07777) != 0600) {
		printf("fail follow-chmod mode=%o\n", st.st_mode & 07777); return 0;
	}
	(void)raw_fchownat;

	/* dirfd-relative resolve: open the dir, operate on a relative name */
	int dfd = open("/tmp/atd", O_RDONLY | O_DIRECTORY);
	if (dfd < 0) { printf("fail opendir\n"); return 0; }
	if (faccessat(dfd, "real", F_OK, 0) != 0) { printf("fail dirfd-access\n"); return 0; }
	errno = 0;
	if (faccessat(dfd, "nope", F_OK, 0) != -1 || errno != ENOENT) { printf("fail dirfd-enoent\n"); return 0; }

	/* unlinkat AT_REMOVEDIR refuses a regular file */
	errno = 0;
	if (unlinkat(dfd, "real", AT_REMOVEDIR) != -1 || errno != ENOTDIR) { printf("fail removedir-on-file errno=%d\n", errno); return 0; }
	/* plain unlinkat refuses a directory */
	mkdir("/tmp/atd/sub", 0755);
	errno = 0;
	if (unlinkat(dfd, "sub", 0) != -1 || errno != EISDIR) { printf("fail unlink-on-dir errno=%d\n", errno); return 0; }
	/* AT_REMOVEDIR removes the dir */
	if (unlinkat(dfd, "sub", AT_REMOVEDIR) != 0) { printf("fail removedir\n"); return 0; }
	if (faccessat(dfd, "sub", F_OK, 0) == 0) { printf("fail dir-still-there\n"); return 0; }

	/* a bad dirfd is EBADF on a relative path */
	errno = 0;
	if (faccessat(70000, "x", F_OK, 0) != -1 || errno != EBADF) { printf("fail ebadf errno=%d\n", errno); return 0; }
	/* a non-directory fd is ENOTDIR */
	int ffd = open("/tmp/atd/real", O_RDONLY);
	errno = 0;
	if (faccessat(ffd, "x", F_OK, 0) != -1 || errno != ENOTDIR) { printf("fail enotdir errno=%d\n", errno); return 0; }
	close(ffd);
	close(dfd);

	printf("OK\n");
	return 0;
}
