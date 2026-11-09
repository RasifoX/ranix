/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* File access by root is decided by capabilities, not by uid 0 (Linux
 * generic_permission, inode_owner_or_capable, chown_ok, check_sticky):
 *  - without CAP_DAC_OVERRIDE and CAP_DAC_READ_SEARCH, root is held to the mode
 *    bits of a file it does not own;
 *  - CAP_DAC_READ_SEARCH alone reads files and searches directories, but does
 *    not write;
 *  - without CAP_FOWNER, root may not chmod another user's file or remove it
 *    from someone else's sticky directory; without CAP_CHOWN, not chown it;
 *  - even with every capability, a file with no x bit is not executable.
 * Each case runs in its own child. Prints FAIL lines, then OK or FAIL <n>.
 * Runs as root. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define DIR_    "/tmp/dac_d"
#define FILE_   "/tmp/dac_d/f"
#define LOOSE   "/tmp/dac_f"
#define STICKY  "/tmp/dac_s"
#define INSIDE  "/tmp/dac_s/g"
#define USER    4321
#define USER2   4322

#define CAP_CHOWN           0
#define CAP_DAC_OVERRIDE    1
#define CAP_DAC_READ_SEARCH 2
#define CAP_FOWNER          3

typedef struct { unsigned version; int pid; } cap_hdr;
typedef struct { unsigned effective, permitted, inheritable; } cap_data;

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s (errno=%d)\n", what, errno); fflush(stdout); fails++; } } while (0)

static int drop_caps(unsigned mask)
{
	cap_hdr h = { 0x20080522, 0 };
	cap_data d[2];
	if (syscall(SYS_capget, &h, d) != 0) return -1;
	d[0].effective &= ~mask;
	d[0].permitted &= ~mask;
	return (int)syscall(SYS_capset, &h, d);
}

static int opens(const char *p, int flags)
{
	int fd = open(p, flags, 0600);
	if (fd < 0) return 0;
	close(fd);
	return 1;
}

static int case_no_dac(void)
{
	if (drop_caps(1u << CAP_DAC_OVERRIDE | 1u << CAP_DAC_READ_SEARCH)) { puts("FAIL setup capset"); return 1; }
	errno = 0;
	CHECK(!opens(LOOSE, O_RDONLY) && errno == EACCES, "no DAC caps: read of a 0600 user file allowed");
	errno = 0;
	CHECK(!opens(LOOSE, O_WRONLY) && errno == EACCES, "no DAC caps: write of a 0600 user file allowed");
	struct stat st;
	errno = 0;
	CHECK(stat(FILE_, &st) == -1 && errno == EACCES, "no DAC caps: search of a 0700 user directory allowed");
	errno = 0;
	CHECK(access(LOOSE, R_OK) == -1 && errno == EACCES, "no DAC caps: access(R_OK) allowed");
	return fails;
}

static int case_read_search(void)
{
	if (drop_caps(1u << CAP_DAC_OVERRIDE)) { puts("FAIL setup capset"); return 1; }
	CHECK(opens(LOOSE, O_RDONLY), "CAP_DAC_READ_SEARCH: read refused");
	errno = 0;
	CHECK(!opens(LOOSE, O_WRONLY) && errno == EACCES, "CAP_DAC_READ_SEARCH: write allowed");
	struct stat st;
	CHECK(stat(FILE_, &st) == 0, "CAP_DAC_READ_SEARCH: search refused");
	errno = 0;
	CHECK(!opens(DIR_ "/new", O_CREAT | O_WRONLY) && errno == EACCES,
	      "CAP_DAC_READ_SEARCH: create in a 0700 user directory allowed");
	return fails;
}

static int case_no_fowner(void)
{
	if (drop_caps(1u << CAP_FOWNER)) { puts("FAIL setup capset"); return 1; }
	errno = 0;
	CHECK(chmod(LOOSE, 0644) == -1 && errno == EPERM, "no CAP_FOWNER: chmod of a user file allowed");
	errno = 0;
	CHECK(unlink(INSIDE) == -1 && errno == EPERM, "no CAP_FOWNER: unlink in another user's sticky dir allowed");
	return fails;
}

static int case_no_chown(void)
{
	if (drop_caps(1u << CAP_CHOWN)) { puts("FAIL setup capset"); return 1; }
	errno = 0;
	CHECK(chown(LOOSE, 0, 0) == -1 && errno == EPERM, "no CAP_CHOWN: chown of a user file allowed");
	return fails;
}

static int case_full(void)
{
	CHECK(opens(LOOSE, O_RDWR), "full caps: open O_RDWR refused");
	CHECK(chmod(LOOSE, 0600) == 0, "full caps: chmod refused");
	errno = 0;
	CHECK(access(LOOSE, X_OK) == -1 && errno == EACCES, "full caps: X_OK on a file with no x bit");
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

static void cleanup(void)
{
	unlink(FILE_); unlink(DIR_ "/new"); rmdir(DIR_);
	unlink(LOOSE); unlink(INSIDE); rmdir(STICKY);
}

static int setup(void)
{
	cleanup();
	if (mkdir(DIR_, 0700) || mkdir(STICKY, 01777)) return -1;
	int a = open(FILE_, O_CREAT | O_WRONLY, 0600), b = open(LOOSE, O_CREAT | O_WRONLY, 0600);
	int c = open(INSIDE, O_CREAT | O_WRONLY, 0600);
	if (a < 0 || b < 0 || c < 0) return -1;
	close(a); close(b); close(c);
	if (chmod(STICKY, 01777) || chown(STICKY, USER2, USER2)) return -1;
	if (chown(FILE_, USER, USER) || chown(DIR_, USER, USER) || chown(LOOSE, USER, USER) ||
	    chown(INSIDE, USER, USER))
		return -1;
	return 0;
}

int main(void)
{
	if (geteuid() != 0) { puts("FAIL run as root"); return 0; }
	if (setup()) { printf("FAIL setup (errno=%d)\n", errno); cleanup(); return 0; }
	run("no-dac", case_no_dac);
	run("read-search", case_read_search);
	run("no-fowner", case_no_fowner);
	run("no-chown", case_no_chown);
	run("full", case_full);
	cleanup();
	if (fails) printf("FAIL %d\n", fails); else puts("OK");
	return 0;
}
