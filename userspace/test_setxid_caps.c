/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The right to set any uid or gid comes from CAP_SETUID / CAP_SETGID, not from
 * the numbers themselves:
 *  - a process with euid != 0 but egid == 0 has no CAP_SETGID, so it may only
 *    move its gids inside its own {r,e,s}gid set;
 *  - root that dropped CAP_SETUID (or CAP_SETGID) with capset() is held to the
 *    same rule as a user;
 *  - root that still holds CAP_SETGID may set any gid even when its egid is no
 *    longer 0.
 * Each case runs in its own child. Prints FAIL lines, then OK or FAIL <n>.
 * Runs as root. */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define USER 4321
#define OTHER 4242

#define CAP_SETGID 6
#define CAP_SETUID 7

typedef struct { unsigned version; int pid; } cap_hdr;
typedef struct { unsigned effective, permitted, inheritable; } cap_data;

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (errno=%d)\n", what, errno); fflush(stdout); fails++; } } while (0)

/* clear one capability from the effective and permitted sets */
static int drop_cap(int cap)
{
	cap_hdr h = { 0x20080522, 0 };
	cap_data d[2];
	if (syscall(SYS_capget, &h, d) != 0) return -1;
	d[cap / 32].effective &= ~(1u << (cap % 32));
	d[cap / 32].permitted &= ~(1u << (cap % 32));
	return (int)syscall(SYS_capset, &h, d);
}

/* euid becomes a user, egid stays 0: setresgid to a foreign gid must fail */
static int case_egid0(void)
{
	if (setgroups(0, NULL) || setresuid(USER, USER, USER)) { puts("FAIL setup egid0"); return 1; }
	errno = 0;
	CHECK(setresgid(OTHER, OTHER, OTHER) == -1 && errno == EPERM, "egid 0 without CAP_SETGID: setresgid(foreign) allowed");
	errno = 0;
	CHECK(setgid(OTHER) == -1 && errno == EPERM, "egid 0 without CAP_SETGID: setgid(foreign) allowed");
	errno = 0;
	CHECK(setregid(OTHER, -1) == -1 && errno == EPERM, "egid 0 without CAP_SETGID: setregid(foreign) allowed");
	return fails;
}

/* root without CAP_SETUID: setuid/setresuid to a foreign uid must fail */
static int case_root_nosetuid(void)
{
	if (drop_cap(CAP_SETUID)) { puts("FAIL setup capset CAP_SETUID"); return 1; }
	errno = 0;
	CHECK(setuid(USER) == -1 && errno == EPERM, "root without CAP_SETUID: setuid(user) allowed");
	errno = 0;
	CHECK(setresuid(USER, USER, USER) == -1 && errno == EPERM, "root without CAP_SETUID: setresuid(user) allowed");
	CHECK(geteuid() == 0, "root without CAP_SETUID: euid changed");
	return fails;
}

/* root without CAP_SETGID: setgid to a foreign gid must fail */
static int case_root_nosetgid(void)
{
	if (drop_cap(CAP_SETGID)) { puts("FAIL setup capset CAP_SETGID"); return 1; }
	errno = 0;
	CHECK(setgid(OTHER) == -1 && errno == EPERM, "root without CAP_SETGID: setgid(foreign) allowed");
	errno = 0;
	CHECK(setresgid(OTHER, OTHER, OTHER) == -1 && errno == EPERM, "root without CAP_SETGID: setresgid(foreign) allowed");
	return fails;
}

/* root with CAP_SETGID but egid already moved off 0: may still set any gid */
static int case_root_egid_user(void)
{
	if (setresgid(USER, USER, USER)) { puts("FAIL setup setresgid(user)"); return 1; }
	CHECK(setresgid(OTHER, OTHER, OTHER) == 0, "root with CAP_SETGID, egid!=0: setresgid(foreign) refused");
	CHECK(setgid(0) == 0 && getegid() == 0, "root with CAP_SETGID, egid!=0: setgid(0) refused");
	return fails;
}

static void run(const char *name, int (*fn)(void))
{
	pid_t p = fork();
	if (p == 0) { fflush(stdout); _exit(fn()); }
	int st = 0;
	waitpid(p, &st, 0);
	if (!WIFEXITED(st)) { printf("FAIL %s crashed\n", name); fails++; }
	else fails += WEXITSTATUS(st);
}

int main(void)
{
	if (geteuid() != 0) { puts("FAIL run as root"); return 0; }
	setvbuf(stdout, NULL, _IONBF, 0);
	run("egid0", case_egid0);
	run("root-nosetuid", case_root_nosetuid);
	run("root-nosetgid", case_root_nosetgid);
	run("root-egid-user", case_root_egid_user);
	if (fails) printf("FAIL %d\n", fails); else puts("OK");
	return 0;
}
