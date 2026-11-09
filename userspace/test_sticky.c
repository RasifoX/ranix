/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A sticky directory: a name goes only by its file's owner, the directory's owner
 * or root, through unlink, rmdir and rename, on either side of a rename, and on
 * each in-memory filesystem. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include "asess_test.h"

#define OTHER 1001

static char D[64];

static void path(char *b, const char *name)
{
	snprintf(b, 96, "%s/%s", D, name);
}

static void mkfile(const char *name, uid_t uid)
{
	char b[96];
	path(b, name);
	int fd = open(b, O_CREAT | O_WRONLY, 0666);
	CHECK(fd >= 0 && fchown(fd, uid, uid) == 0, "create");
	close(fd);
}

static void user_side(void)
{
	char a[96], b[96];
	path(a, "others"); path(b, "moved");
	DENIED(unlink(a), EPERM, "unlink another's file");
	DENIED(rename(a, b), EPERM, "rename another's file away");
	path(b, "mine");
	DENIED(rename(b, a), EPERM, "rename over another's file");
	path(a, "othersdir");
	DENIED(rmdir(a), EPERM, "rmdir another's directory");
	path(a, "nosuch");
	DENIED(unlink(a), ENOENT, "a name that is not there");

	/* relative, as rm and mv in the directory do it */
	CHECK(chdir(D) == 0, "chdir");
	DENIED(unlink("others"), EPERM, "relative unlink of another's file");
	CHECK(rename("mine", "mine2") == 0, "rename its own file");
	CHECK(unlink("mine2") == 0, "unlink its own file");
	CHECK(mkdir("reldir", 0755) == 0, "relative mkdir");
	CHECK(rmdir("reldir") == 0, "relative rmdir");
	CHECK(symlink("/x", "rellink") == 0, "relative symlink");
	CHECK(unlink("rellink") == 0, "relative unlink of a symlink");
	CHECK(open("rel", O_CREAT | O_WRONLY, 0644) >= 0, "relative create");
	CHECK(rename("rel", "rel2") == 0 && unlink("rel2") == 0, "its own, relative");
}

static void owner_side(void)
{
	char a[96];
	path(a, "others2");
	CHECK(unlink(a) == 0, "the directory's owner may unlink");
}

static void run(const char *dir)
{
	char a[96];
	strcpy(D, dir);
	CHECK(mkdir(D, 0777) == 0 && chmod(D, 01777) == 0, "sticky directory");
	mkfile("others", OTHER);
	mkfile("mine", UID);
	path(a, "othersdir");
	CHECK(mkdir(a, 0755) == 0 && chown(a, OTHER, OTHER) == 0, "another's directory");

	pid_t c = fork();
	if (c == 0) {
		if (setgid(GID) || setuid(UID)) _exit(1);
		fails = 0;
		user_side();
		_exit(fails ? 2 : 0);
	}
	sess_reap(c);

	mkfile("others2", OTHER);
	CHECK(chown(D, UID, GID) == 0, "give the directory away");
	c = fork();
	if (c == 0) {
		if (setgid(GID) || setuid(UID)) _exit(1);
		fails = 0;
		owner_side();
		_exit(fails ? 2 : 0);
	}
	sess_reap(c);

	path(a, "others");
	CHECK(unlink(a) == 0, "root may unlink");
	path(a, "othersdir");
	CHECK(rmdir(a) == 0, "root may rmdir");
	CHECK(rmdir(D) == 0, "cleanup");
}

int main(void)
{
	if (getuid() != 0) { puts("SKIP needs root"); return 0; }
	run("/tmp/stick");
	mkdir("/tmp/stm", 0755);
	CHECK(mount("t", "/tmp/stm", "tmpfs", 0, NULL) == 0, "mount");
	run("/tmp/stm/d");
	CHECK(umount("/tmp/stm") == 0, "umount");
	return finish();
}
