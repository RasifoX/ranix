/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A new pipe holds 64 KB, as on Linux (LTP fcntl35, fcntl37); an opener
 * without CAP_SYS_RESOURCE gets no more than /proc/sys/fs/pipe-max-size.
 * A full pipe cannot shrink below what it holds (EBUSY). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int bad;

static long setmax(long v)   /* returns the old pipe-max-size, -1 if it cannot */
{
	long old = -1;
	FILE *f = fopen("/proc/sys/fs/pipe-max-size", "r");
	if (!f || fscanf(f, "%ld", &old) != 1) { if (f) fclose(f); return -1; }
	fclose(f);
	f = fopen("/proc/sys/fs/pipe-max-size", "w");
	if (!f) return -1;
	fprintf(f, "%ld\n", v);
	if (fclose(f)) return -1;
	return old;
}

int main(void)
{
	int p[2];
	if (pipe2(p, O_NONBLOCK)) { puts("pipe2 failed"); return 1; }
	long sz = fcntl(p[1], F_GETPIPE_SZ);
	if (sz != 65536) { printf("new pipe holds %ld, want 65536\n", sz); bad++; }
	static char buf[4096];
	long total = 0, n;
	while ((n = write(p[1], buf, sizeof(buf))) > 0) total += n;
	if (total != sz) { printf("wrote %ld into a pipe of %ld\n", total, sz); bad++; }
	errno = 0;
	if (fcntl(p[1], F_SETPIPE_SZ, sz / 2) != -1 || errno != EBUSY) {
		printf("shrinking a full pipe to half: errno %d, want EBUSY\n", errno); bad++;
	}
	close(p[0]); close(p[1]);

	if (geteuid() == 0) {
		long old = setmax(4096);
		if (old < 0) { puts("cannot set pipe-max-size"); bad++; }
		pid_t c = fork();
		if (c == 0) {
			if (setuid(65534)) _exit(2);
			if (pipe(p)) _exit(2);
			long s = fcntl(p[1], F_GETPIPE_SZ);
			if (s != 4096) { printf("unprivileged pipe with pipe-max-size 4096 holds %ld\n", s); _exit(1); }
			_exit(0);
		}
		int st;
		waitpid(c, &st, 0);
		if (!WIFEXITED(st) || WEXITSTATUS(st)) bad++;
		if (pipe(p) == 0) {
			long s = fcntl(p[1], F_GETPIPE_SZ);
			if (s != 65536) { printf("root's pipe with pipe-max-size 4096 holds %ld, want 65536\n", s); bad++; }
			close(p[0]); close(p[1]);
		}
		if (old > 0) setmax(old);
	}
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
