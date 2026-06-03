/* statx flag semantics: AT_SYMLINK_NOFOLLOW must report the symlink itself (not
 * its target) — ls -l / find rely on this — and AT_EMPTY_PATH stats the open fd.
 * Without the fix statx always followed and ignored the fd. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/stat.h>

static int do_statx(int dirfd, const char *path, int flags, struct statx *b)
{
	return (int)syscall(SYS_statx, dirfd, path, flags, STATX_BASIC_STATS, b);
}

int main(void)
{
	unlink("/tmp/sx_link");
	int fd = open("/tmp/sx_file", O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) { printf("statx-fail create\n"); return 0; }
	write(fd, "hello", 5);
	close(fd);
	if (symlink("/tmp/sx_file", "/tmp/sx_link") != 0) { printf("statx-fail symlink\n"); return 0; }

	struct statx sx;

	/* follow (default): link resolves to the regular target */
	if (do_statx(AT_FDCWD, "/tmp/sx_link", 0, &sx) != 0) { printf("statx-fail follow\n"); return 0; }
	if (!S_ISREG(sx.stx_mode) || sx.stx_size != 5) { printf("statx-fail follow-mode %o sz=%llu\n", sx.stx_mode, (unsigned long long)sx.stx_size); return 0; }

	/* nofollow: report the symlink itself */
	if (do_statx(AT_FDCWD, "/tmp/sx_link", AT_SYMLINK_NOFOLLOW, &sx) != 0) { printf("statx-fail nofollow\n"); return 0; }
	if (!S_ISLNK(sx.stx_mode)) { printf("statx-fail nofollow-mode %o\n", sx.stx_mode); return 0; }

	/* AT_EMPTY_PATH: stat the open fd, compare ino with a path-based statx */
	int rfd = open("/tmp/sx_file", O_RDONLY);
	if (rfd < 0) { printf("statx-fail reopen\n"); return 0; }
	struct statx sf, sp;
	int e = do_statx(rfd, "", AT_EMPTY_PATH, &sf);
	close(rfd);
	if (e != 0) { printf("statx-fail emptypath\n"); return 0; }
	if (do_statx(AT_FDCWD, "/tmp/sx_file", 0, &sp) != 0) { printf("statx-fail bypath\n"); return 0; }
	if (!S_ISREG(sf.stx_mode) || sf.stx_ino != sp.stx_ino) { printf("statx-fail fd-ino\n"); return 0; }

	printf("OK\n");
	return 0;
}
