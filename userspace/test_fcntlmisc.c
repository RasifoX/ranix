/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* fcntl's pipe, owner and lease commands as Linux answers them (LTP fcntl27,
 * fcntl30, fcntl31, fcntl37): a pipe holds exactly F_GETPIPE_SZ bytes and
 * F_SETPIPE_SZ resizes it (EBUSY below what it holds, EBADF on a non-pipe);
 * F_SETOWN/F_SETSIG/F_SETOWN_EX read back; an O_ASYNC read end sends its
 * owner the F_SETSIG signal when data comes; a read lease on a file open for
 * writing is EAGAIN and F_GETLEASE reports none. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int bad;
#define CHECK(c, ...) do { if (!(c)) { printf(__VA_ARGS__); putchar('\n'); bad++; } } while (0)

/* bytes a nonblocking writer gets into an empty pipe */
static long fill(int w, long upto)
{
	char *b = calloc(1, (size_t)upto);
	long n = 0;
	while (n < upto) {
		long r = write(w, b + n, (size_t)(upto - n));
		if (r <= 0) break;
		n += r;
	}
	free(b);
	return n;
}

int main(void)
{
	int p[2];
	pipe2(p, O_NONBLOCK);
	long sz = fcntl(p[1], F_GETPIPE_SZ);
	CHECK(sz > 0, "F_GETPIPE_SZ: %ld (%s)", sz, strerror(errno));
	CHECK(fill(p[1], sz + 1) == sz, "an empty pipe of %ld bytes did not take exactly that many", sz);
	close(p[0]); close(p[1]);

	/* 64 KB full, then half of that: the data no longer fits */
	pipe2(p, O_NONBLOCK);
	fcntl(p[1], F_SETPIPE_SZ, 65536);
	fill(p[1], 65536);
	errno = 0;
	CHECK(fcntl(p[1], F_SETPIPE_SZ, 32768) < 0 && errno == EBUSY, "shrinking below its data: not EBUSY");
	close(p[0]); close(p[1]);

	pipe2(p, O_NONBLOCK);
	long big = fcntl(p[1], F_SETPIPE_SZ, 65536);
	CHECK(big >= 65536 && fcntl(p[0], F_GETPIPE_SZ) == big, "F_SETPIPE_SZ 65536: %ld", big);
	CHECK(fill(p[1], big + 1) == big, "a %ld-byte pipe did not take %ld bytes", big, big);
	char *rd = malloc((size_t)big);
	CHECK(read(p[0], rd, (size_t)big) == big, "reading back the full pipe");
	free(rd);
	close(p[0]); close(p[1]);

	int fd = open("/tmp/fcntlmisc", O_CREAT | O_RDWR | O_TRUNC, 0644);
	errno = 0;
	CHECK(fcntl(fd, F_SETPIPE_SZ, 65536) < 0 && errno == EBADF, "F_SETPIPE_SZ on a file: not EBADF");
	FILE *pm = fopen("/proc/sys/fs/pipe-max-size", "r");
	long pmax = 0;
	CHECK(pm && fscanf(pm, "%ld", &pmax) == 1 && pmax >= 65536, "/proc/sys/fs/pipe-max-size: %ld", pmax);
	if (pm) fclose(pm);

	errno = 0;
	CHECK(fcntl(fd, F_SETLEASE, F_RDLCK) < 0 && errno == EAGAIN, "read lease on a writable file: not EAGAIN");
	CHECK(fcntl(fd, F_GETLEASE) == F_UNLCK, "F_GETLEASE: %d", fcntl(fd, F_GETLEASE));

	CHECK(fcntl(fd, F_SETOWN, getpid()) == 0 && fcntl(fd, F_GETOWN) == getpid(), "F_SETOWN/F_GETOWN");
	CHECK(fcntl(fd, F_SETSIG, SIGUSR2) == 0 && fcntl(fd, F_GETSIG) == SIGUSR2, "F_SETSIG/F_GETSIG");
	struct f_owner_ex ex = { F_OWNER_PID, getpid() }, got = { 0, 0 };
	CHECK(fcntl(fd, F_SETOWN_EX, &ex) == 0 && fcntl(fd, F_GETOWN_EX, &got) == 0 &&
	      got.type == F_OWNER_PID && got.pid == getpid(), "F_SETOWN_EX/F_GETOWN_EX");
	close(fd);
	unlink("/tmp/fcntlmisc");

	/* O_ASYNC: data in the pipe signals the read end's owner with F_SETSIG's signal */
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGIO);
	sigprocmask(SIG_BLOCK, &set, NULL);
	pipe(p);
	fcntl(p[0], F_SETOWN, getpid());
	fcntl(p[0], F_SETSIG, SIGUSR1);
	CHECK(fcntl(p[0], F_SETFL, O_ASYNC) == 0 && (fcntl(p[0], F_GETFL) & O_ASYNC), "F_SETFL O_ASYNC");
	pid_t k = fork();
	if (k == 0) { usleep(100000); write(p[1], "x", 1); _exit(0); }
	struct timespec ts = { 3, 0 };
	int sig = sigtimedwait(&set, NULL, &ts);
	CHECK(sig == SIGUSR1, "O_ASYNC pipe: got signal %d, want SIGUSR1", sig);
	waitpid(k, NULL, 0);
	close(p[0]); close(p[1]);

	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
