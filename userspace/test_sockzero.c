/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A socket has no file position, and a read of nothing is no wait (Linux).
 * Ranix's read() on a stream socket with data queued and a length of 0 went
 * round its wait loop for good: nothing to copy, "data is queued" so try again.
 * The CPU was held in the kernel past SIGKILL (found by test_fuzz, via pread64,
 * which Ranix let through to a socket as a seek plus a read). Now:
 *   read(fd, buf, 0) on a stream socket with data queued is 0 at once, data kept;
 *   pread, pwrite and lseek on a socket are ESPIPE, as for a pipe.
 * Each case runs in a child with a time limit. Prints OK or FAIL lines. */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;

static int sock_with_data(int sv[2])
{
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) return -1;
	return write(sv[0], "abc", 3) == 3 ? 0 : -1;
}

/* 0: passed; else the failure is printed by the child */
static int case_zero_read(void)
{
	int sv[2];
	char b[8];
	if (sock_with_data(sv)) { printf("FAIL: zero read: setup: %s\n", strerror(errno)); return 1; }
	ssize_t r = read(sv[1], b, 0);
	if (r != 0) { printf("FAIL: zero read returned %zd (errno %d)\n", r, errno); return 1; }
	r = read(sv[1], b, sizeof(b));
	if (r != 3 || memcmp(b, "abc", 3)) { printf("FAIL: zero read lost the data (%zd)\n", r); return 1; }
	return 0;
}

static int case_zero_recv(void)
{
	int sv[2];
	char b[8];
	if (sock_with_data(sv)) { printf("FAIL: zero recv: setup: %s\n", strerror(errno)); return 1; }
	ssize_t r = recv(sv[1], b, 0, 0);
	if (r != 0) { printf("FAIL: zero recv returned %zd (errno %d)\n", r, errno); return 1; }
	return 0;
}

static int case_espipe(void)
{
	int sv[2];
	char b[8];
	int bad = 0;
	if (sock_with_data(sv)) { printf("FAIL: espipe: setup: %s\n", strerror(errno)); return 1; }
	errno = 0;
	ssize_t r = pread(sv[1], b, 1, 0);
	if (r != -1 || errno != ESPIPE) { printf("FAIL: pread on a socket: %zd errno %d, want ESPIPE\n", r, errno); bad = 1; }
	errno = 0;
	r = pread(sv[1], b, 0, 0);
	if (r != -1 || errno != ESPIPE) { printf("FAIL: zero pread on a socket: %zd errno %d, want ESPIPE\n", r, errno); bad = 1; }
	errno = 0;
	r = pwrite(sv[0], "x", 1, 0);
	if (r != -1 || errno != ESPIPE) { printf("FAIL: pwrite on a socket: %zd errno %d, want ESPIPE\n", r, errno); bad = 1; }
	errno = 0;
	off_t o = lseek(sv[1], 0, SEEK_CUR);
	if (o != -1 || errno != ESPIPE) { printf("FAIL: lseek on a socket: %lld errno %d, want ESPIPE\n", (long long)o, errno); bad = 1; }
	struct iovec v = { b, 1 };
	errno = 0;
	r = preadv(sv[1], &v, 1, 0);
	if (r != -1 || errno != ESPIPE) { printf("FAIL: preadv on a socket: %zd errno %d, want ESPIPE\n", r, errno); bad = 1; }
	r = read(sv[1], b, sizeof(b));
	if (r != 3 || memcmp(b, "abc", 3)) { printf("FAIL: positioned calls disturbed the data (%zd)\n", r); bad = 1; }
	return bad;
}

static void run(const char *name, int (*fn)(void))
{
	fflush(stdout);
	pid_t c = fork();
	if (c == 0) { alarm(3); _exit(fn()); }
	int st = 0;
	waitpid(c, &st, 0);
	if (WIFSIGNALED(st)) { printf("FAIL: %s: killed by signal %d (hung?)\n", name, WTERMSIG(st)); fails++; }
	else if (WEXITSTATUS(st)) fails++;
}

int main(void)
{
	run("zero read", case_zero_read);
	run("zero recv", case_zero_recv);
	run("espipe", case_espipe);
	printf(fails ? "FAILED %d\n" : "OK\n", fails);
	return fails != 0;
}
