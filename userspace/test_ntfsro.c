/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A mounted NTFS volume refuses every way of changing anything, and its paths behave.
 * usage: test_ntfsro MOUNTPOINT  (the image holds a file "bootmgr" and dir "Users/rasi") */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

static int fails;
static char p[1024], q[1024];

#define AT(s) (snprintf(p, sizeof(p), "%s/%s", root, s), p)

static void erofs(const char *what, long got)
{
	int ok = got < 0 && (errno == EROFS);
	printf("%s: %s (ret %ld errno %d)\n", ok ? "PASS" : "FAIL", what, got, errno);
	if (!ok) fails++;
}

static void check(const char *what, int ok)
{
	printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
	if (!ok) fails++;
}

int main(int argc, char **argv)
{
	if (argc < 2) return 2;
	const char *root = argv[1];
	struct stat st, st2;
	check("stat of a file", stat(AT("bootmgr"), &st) == 0 && st.st_size == 413738);
	check("files are root only", (st.st_mode & 0777) == 0400);

	erofs("open O_WRONLY", open(AT("bootmgr"), O_WRONLY));
	erofs("open O_RDWR", open(AT("bootmgr"), O_RDWR));
	erofs("open O_TRUNC", open(AT("bootmgr"), O_RDONLY | O_TRUNC));
	erofs("create", open(AT("new.txt"), O_CREAT | O_WRONLY, 0644));
	erofs("mkdir", mkdir(AT("newdir"), 0755));
	erofs("rmdir", rmdir(AT("Users")));
	erofs("unlink", unlink(AT("bootmgr")));
	erofs("mknod", mknod(AT("node"), S_IFIFO | 0644, 0));
	snprintf(q, sizeof(q), "%s/x", root);
	erofs("rename", rename(AT("bootmgr"), q));
	erofs("link", link(AT("bootmgr"), q));
	erofs("symlink", symlink("bootmgr", AT("sl")));
	erofs("chmod", chmod(AT("bootmgr"), 0777));
	erofs("chown", chown(AT("bootmgr"), 1, 1));
	erofs("truncate", truncate(AT("bootmgr"), 0));
	struct timespec ts[2] = { {0, 0}, {0, 0} };
	erofs("utimensat", utimensat(AT_FDCWD, AT("bootmgr"), ts, 0));
	erofs("setxattr", setxattr(AT("bootmgr"), "user.x", "y", 1, 0));
	erofs("removexattr", removexattr(AT("bootmgr"), "user.x"));

	int fd = open(AT("bootmgr"), O_RDONLY);
	check("open read-only", fd >= 0);
	errno = 0;
	long w = write(fd, "x", 1);
	check("write on a read fd", w < 0);
	check("ftruncate", ftruncate(fd, 0) < 0);
	void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	check("shared writable mmap", m == MAP_FAILED);
	close(fd);

	/* a remount must not lift it */
	int rr = mount(NULL, root, NULL, MS_REMOUNT, NULL);
	printf("remount rw returned %d\n", rr);
	erofs("create after remount", open(AT("new2.txt"), O_CREAT | O_WRONLY, 0644));
	erofs("mkdir after remount", mkdir(AT("newdir2"), 0755));

	/* .. inside a deep directory stays in that directory's parent */
	check("Users/rasi/.. is Users", stat(AT("Users/rasi/.."), &st) == 0 &&
	      stat(AT("Users"), &st2) == 0 && st.st_ino == st2.st_ino);
	check("Users/rasi/Belgeler/../.. is Users", stat(AT("Users/rasi/Belgeler/../.."), &st) == 0 &&
	      st.st_ino == st2.st_ino);
	/* above the mount root stays the normal parent */
	check("Users/.. is the root", stat(AT("Users/.."), &st) == 0 &&
	      stat(root, &st2) == 0 && st.st_ino == st2.st_ino);

	/* by record number */
	snprintf(q, sizeof(q), "%s/.ntfs-id/5", root);
	check(".ntfs-id/5 is the root", stat(q, &st) == 0 && st.st_ino == st2.st_ino);
	snprintf(q, sizeof(q), "%s/.ntfs-id/99999999999", root);
	check("a record past the end is ENOENT", stat(q, &st) != 0 && errno == ENOENT);
	snprintf(q, sizeof(q), "%s/.ntfs-id/12x", root);
	check("not a number is ENOENT", stat(q, &st) != 0 && errno == ENOENT);

	/* links stay names of nothing */
	char t[2048];
	ssize_t n = readlink(AT("reparse/sym-escape"), t, sizeof(t) - 1);
	if (n > 0) t[n] = 0;
	check("escaping symlink is one harmless component", n > 0 && !strchr(t, '/'));
	check("following it finds nothing", stat(AT("reparse/sym-escape"), &st) != 0);
	check("a junction is not entered", stat(AT("reparse/junction/x"), &st) != 0);
	n = getxattr(AT("reparse/wof.bin"), "user.ntfs.unreadable", t, sizeof(t));
	check("WOF file says so", n == 3 && !memcmp(t, "wof", 3));
	fd = open(AT("reparse/wof.bin"), O_RDONLY);
	errno = 0;
	check("reading it fails rather than returning zeros", fd >= 0 && read(fd, t, 16) < 0 && errno == EOPNOTSUPP);
	close(fd);

	/* the listing hides the id directory and the metafiles */
	DIR *d = opendir(root);
	int hidden = 0, count = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		count++;
		if (!strcmp(e->d_name, ".ntfs-id") || e->d_name[0] == '$') hidden++;
	}
	closedir(d);
	check("root listing", count > 5 && !hidden);
	printf("ntfsro %s fails=%d\n", fails ? "FAILED" : "OK", fails);
	return fails != 0;
}
