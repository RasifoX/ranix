/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* rmdir racing a name that turns from a directory into a file. rmdir looked the
 * name up (a directory), then listed it to see it empty; a file put under the
 * name in between was listed as a directory, its data pages read as list
 * entries, and the kernel took a page fault (found by test_fuzz). Two processes
 * on one name for a few seconds: one cycles mkdir, rmdir, creat, unlink, the
 * other rmdirs. The kernel must stay up, and rmdir must fail on a file with
 * ENOTDIR whenever it sees one. Prints OK or FAIL lines. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DIR_ "/tmp/rmdirrace"
#define NAME DIR_ "/x"

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int main(void)
{
	mkdir(DIR_, 0755);
	pid_t c = fork();
	if (c == 0) {
		for (;;) {
			mkdir(NAME, 0755);
			rmdir(NAME);
			int fd = open(NAME, O_CREAT | O_WRONLY, 0644);
			if (fd >= 0) { if (write(fd, "data", 4) != 4) {} close(fd); }
			unlink(NAME);
		}
	}
	long calls = 0, bad = 0;
	double end = now() + 4;
	while (now() < end) {
		if (rmdir(NAME) != 0 && errno != ENOENT && errno != ENOTDIR && errno != ENOTEMPTY) {
			if (!bad) printf("FAIL: rmdir: %s\n", strerror(errno));
			bad++;
		}
		calls++;
	}
	kill(c, SIGKILL);
	waitpid(c, NULL, 0);
	unlink(NAME);
	rmdir(NAME);
	rmdir(DIR_);
	printf("calls %ld\n", calls);
	printf(bad ? "FAILED\n" : "OK\n");
	return bad != 0;
}
