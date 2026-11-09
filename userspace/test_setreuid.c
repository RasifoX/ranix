/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* setreuid / setregid / setresgid for an unprivileged process (POSIX and Linux):
 *  - the real id may become only the real or effective id, never the saved one;
 *  - the effective id may become the real, effective or saved id;
 *  - the saved id becomes the new effective id only when the real id is set, or
 *    the effective id is set to something other than the previous REAL id.
 *    So setreuid(-1, getuid()) is a temporary drop and setreuid(-1, saved) a
 *    way back; programs written for Linux rely on that.
 *  - setresgid moves only inside the {r,e,s}gid set; getresuid/getresgid agree.
 * Each case runs in its own child that starts as root and drops all root ids.
 * Prints FAIL lines, then OK or FAIL <n>. Runs as root. */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (errno=%d)\n", what, errno); fails++; } } while (0)

static int uids_are(uid_t r, uid_t e, uid_t s)
{
	uid_t a, b, c;
	return getresuid(&a, &b, &c) == 0 && a == r && b == e && c == s;
}

static int gids_are(gid_t r, gid_t e, gid_t s)
{
	gid_t a, b, c;
	return getresgid(&a, &b, &c) == 0 && a == r && b == e && c == s;
}

/* (1000, 2000, 2000): drop the effective id to the real one and come back */
static int case_uid_temp_drop(void)
{
	if (setgroups(0, NULL) || setresuid(1000, 2000, 2000)) { puts("FAIL setup uid"); return 1; }
	CHECK(setreuid(-1, 1000) == 0, "setreuid(-1, ruid)");
	CHECK(uids_are(1000, 1000, 2000), "setreuid(-1, ruid) changed the saved uid");
	CHECK(setreuid(-1, 2000) == 0, "setreuid(-1, saved) after a temporary drop");
	CHECK(uids_are(1000, 2000, 2000), "uids after the way back");
	errno = 0;
	CHECK(setreuid(-1, 3000) == -1 && errno == EPERM, "setreuid(-1, foreign) allowed");
	errno = 0;
	CHECK(setreuid(3000, -1) == -1 && errno == EPERM, "setreuid(foreign, -1) allowed");
	CHECK(setreuid(2000, -1) == 0, "setreuid(euid, -1)");
	CHECK(uids_are(2000, 2000, 2000), "setreuid(euid, -1): saved must follow the new euid");
	return fails;
}

/* (1000, 1000, 2000): the real uid may not take the saved uid */
static int case_uid_real_from_saved(void)
{
	if (setgroups(0, NULL) || setresuid(1000, 1000, 2000)) { puts("FAIL setup uid2"); return 1; }
	errno = 0;
	CHECK(setreuid(2000, -1) == -1 && errno == EPERM, "setreuid(saved, -1): real uid took the saved uid");
	return fails;
}

/* the same rules for gids, with the uids already dropped */
static int case_gid(void)
{
	if (setgroups(0, NULL) || setresgid(1000, 2000, 2000) || setresuid(4321, 4321, 4321)) {
		puts("FAIL setup gid");
		return 1;
	}
	CHECK(setregid(-1, 1000) == 0, "setregid(-1, rgid)");
	CHECK(gids_are(1000, 1000, 2000), "setregid(-1, rgid) changed the saved gid");
	CHECK(setregid(-1, 2000) == 0, "setregid(-1, saved) after a temporary drop");
	errno = 0;
	CHECK(setregid(3000, -1) == -1 && errno == EPERM, "setregid(foreign, -1) allowed");
	errno = 0;
	CHECK(setresgid(5, -1, -1) == -1 && errno == EPERM, "setresgid(foreign) allowed");
	CHECK(setresgid(2000, 1000, -1) == 0, "setresgid inside the set");
	CHECK(gids_are(2000, 1000, 2000), "getresgid after setresgid");
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
	run("uid-temp-drop", case_uid_temp_drop);
	run("uid-real-from-saved", case_uid_real_from_saved);
	run("gid", case_gid);
	if (fails) printf("FAIL %d\n", fails); else puts("OK");
	return 0;
}
