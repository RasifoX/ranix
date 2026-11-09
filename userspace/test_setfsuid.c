/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* setfsuid / setfsgid: the filesystem ids are what file access is checked
 * against. Root that sets its fsuid to a user must be treated as that user by
 * open() (file servers drop file privileges this way), files it creates belong
 * to the fsuid/fsgid, and the fsuid follows the euid again after a set*uid call.
 * setfsuid always returns the previous fsuid; an invalid id (-1) changes nothing.
 * Prints FAIL lines, then OK or FAIL <n>. Runs as root. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/fsuid.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define SECRET "/tmp/fsuid_secret"
#define DIR_   "/tmp/fsuid_d"          /* 0777: open to the user, whatever /tmp's mode */
#define MADE   DIR_ "/made"
#define USER   4321

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (errno=%d)\n", what, errno); fails++; } } while (0)

static int case_access(void)
{
	CHECK(setfsuid(USER) == 0, "setfsuid(user) did not return the old fsuid 0");
	CHECK(setfsgid(USER) == 0, "setfsgid(user) did not return the old fsgid 0");
	errno = 0;
	int fd = open(SECRET, O_RDONLY);
	CHECK(fd < 0 && errno == EACCES, "fsuid=user still opened a 0600 root file");
	if (fd >= 0) close(fd);
	fd = open(MADE, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	CHECK(fd >= 0, "create in /tmp as fsuid=user");
	if (fd >= 0) close(fd);
	struct stat st;
	CHECK(stat(MADE, &st) == 0 && st.st_uid == USER && st.st_gid == USER,
	      "a new file must belong to fsuid/fsgid");
	CHECK(setfsuid(0) == USER, "setfsuid(0) did not return the old fsuid");
	fd = open(SECRET, O_RDONLY);
	CHECK(fd >= 0, "fsuid back to 0 cannot open the root file");
	if (fd >= 0) close(fd);
	return fails;
}

static int case_invalid(void)
{
	setfsuid(-1);
	CHECK(setfsuid(-1) == 0, "setfsuid(-1) changed the fsuid");
	return fails;
}

static int case_follows_euid(void)
{
	setfsuid(USER + 1);
	if (setgroups(0, NULL) || setresuid(1000, 1000, 1000)) { puts("FAIL setup setresuid"); return 1; }
	CHECK(setfsuid(-1) == 1000, "fsuid does not follow the euid after setresuid");
	errno = 0;
	setfsuid(USER + 1);   /* not one of the user's ids: refused, returns old */
	CHECK(setfsuid(-1) == 1000, "an unprivileged setfsuid(foreign) took effect");
	return fails;
}

static void run(const char *name, int (*fn)(void))
{
	fflush(stdout);
	pid_t p = fork();
	if (p == 0) { int r = fn(); fflush(stdout); _exit(r); }
	int st = 0;
	waitpid(p, &st, 0);
	if (!WIFEXITED(st)) { printf("FAIL %s crashed\n", name); fails++; }
	else fails += WEXITSTATUS(st);
}

int main(void)
{
	if (geteuid() != 0) { puts("FAIL run as root"); return 0; }
	unlink(MADE);
	mkdir(DIR_, 0777);
	if (chmod(DIR_, 0777)) { puts("FAIL setup dir"); return 0; }
	int fd = open(SECRET, O_CREAT | O_WRONLY | O_TRUNC, 0600);
	if (fd < 0 || fchown(fd, 0, 0) != 0) { puts("FAIL setup secret"); return 0; }
	close(fd);
	chmod(SECRET, 0600);
	run("access", case_access);
	run("invalid", case_invalid);
	run("follows-euid", case_follows_euid);
	unlink(SECRET);
	unlink(MADE);
	rmdir(DIR_);
	if (fails) printf("FAIL %d\n", fails); else puts("OK");
	return 0;
}
