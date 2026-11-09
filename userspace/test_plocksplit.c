/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* POSIX record locks of one process split and merge like Linux's: a lock or an
 * unlock over part of an older lock trims it (or cuts a hole in it) instead of
 * dropping all of it, adjacent locks of one type become one, and F_GETLK from
 * another process names the lowest conflicting range. LTP fcntl11/19/20/21. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static int fd, bad;

static void lk(int type, off_t start, off_t len)
{
	struct flock fl = { .l_type = type, .l_whence = SEEK_SET, .l_start = start, .l_len = len };
	if (fcntl(fd, F_SETLK, &fl) < 0) { printf("setlk %d %ld %ld failed\n", type, (long)start, (long)len); bad++; }
}

/* what another process sees when it asks for a write lock from start on */
static void see(const char *what, off_t start, int type, off_t ws, off_t wl)
{
	int p[2];
	pipe(p);
	pid_t k = fork();
	if (k == 0) {
		struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = start, .l_len = 0 };
		fcntl(fd, F_GETLK, &fl);
		write(p[1], &fl, sizeof fl);
		_exit(0);
	}
	struct flock fl;
	read(p[0], &fl, sizeof fl);
	waitpid(k, NULL, 0);
	close(p[0]); close(p[1]);
	if (fl.l_type != type || (type != F_UNLCK && (fl.l_start != ws || fl.l_len != wl))) {
		printf("%s: want type %d at %ld len %ld, got type %d at %ld len %ld\n", what,
		       type, (long)ws, (long)wl, fl.l_type, (long)fl.l_start, (long)fl.l_len);
		bad++;
	}
}

static void unlock_all(void) { lk(F_UNLCK, 0, 0); }

int main(void)
{
	fd = open("/tmp/plocksplit", O_CREAT | O_RDWR | O_TRUNC, 0600);
	write(fd, "0123456789abcdefghijklmnopqrstuvwxyz", 36);

	lk(F_WRLCK, 10, 5); lk(F_RDLCK, 13, 5);          /* write 10-12 stays, read 13-17 */
	see("trim", 0, F_WRLCK, 10, 3);
	see("trim tail", 13, F_RDLCK, 13, 5);
	unlock_all();

	lk(F_WRLCK, 0, 30); lk(F_UNLCK, 10, 10);         /* a hole: 0-9 and 20-29 */
	see("hole head", 0, F_WRLCK, 0, 10);
	see("hole tail", 10, F_WRLCK, 20, 10);
	unlock_all();

	lk(F_RDLCK, 10, 5); lk(F_RDLCK, 15, 5);          /* adjacent: one 10-19 */
	see("merge", 0, F_RDLCK, 10, 10);
	unlock_all();
	see("all unlocked", 0, F_UNLCK, 0, 0);

	unlink("/tmp/plocksplit");
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
