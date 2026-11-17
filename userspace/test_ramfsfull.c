/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Filling the in-memory root filesystem is ENOSPC, not the OOM killer.
 * Ranix's root ramfs took file data with no bound: test_fuzz wrote 1 GB into
 * one file with sendfile and the OOM killer then took the shell that ran it.
 * The root ramfs holds the live system itself, so it cannot be capped at half
 * of RAM as Linux's tmpfs rootfs is; instead file data stops at a reserve of
 * free memory kept for the rest of the system. A writer must get ENOSPC
 * (write and sendfile alike, memfd too), must not be killed, the system must
 * still fork and exec while the file is there, and the memory must come back
 * when the file goes. Prints OK or FAIL lines. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fails;
#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } while (0)

static long meminfo_kb(const char *key)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[128];
	long v = -1;
	size_t kl = strlen(key);
	while (f && fgets(line, sizeof(line), f))
		if (!strncmp(line, key, kl) && line[kl] == ':') { v = atol(line + kl + 1); break; }
	if (f) fclose(f);
	return v;
}

/* write until an error; returns bytes written, *err the errno that stopped it */
static long fill_write(int fd, int *err)
{
	static char b[1 << 20];
	memset(b, 0x5a, sizeof(b));
	long total = 0;
	for (;;) {
		ssize_t r = write(fd, b, sizeof(b));
		if (r < 0) { *err = errno; return total; }
		if (r == 0) { *err = 0; return total; }
		total += r;
	}
}

static long fill_sendfile(int out, int *err)
{
	int in = open("/dev/zero", O_RDONLY);
	if (in < 0) { *err = errno; return 0; }
	long total = 0;
	for (;;) {
		ssize_t r = sendfile(out, in, NULL, 1 << 22);
		if (r < 0) { *err = errno; break; }
		if (r == 0) { *err = 0; break; }
		total += r;
	}
	close(in);
	return total;
}

static int still_alive(void)
{
	pid_t c = fork();
	if (c < 0) return 0;
	if (c == 0) { execl("/bin/true", "true", (char *)NULL); _exit(127); }
	int st = 0;
	waitpid(c, &st, 0);
	return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

/* one filling: run in a child so an OOM kill shows as a signal */
static void one(const char *how, int mode)
{
	long before = meminfo_kb("MemFree");
	fflush(stdout);
	pid_t c = fork();
	if (c == 0) {
		int fd, err = 0;
		long got;
		if (mode == 2) fd = memfd_create("fill", 0);
		else fd = open("/ramfsfull.dat", O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd < 0) { printf("FAIL: %s: open: %s\n", how, strerror(errno)); _exit(1); }
		got = mode == 1 ? fill_sendfile(fd, &err) : fill_write(fd, &err);
		int bad = 0;
		if (err != ENOSPC) { printf("FAIL: %s: stopped by errno %d after %ld MB, want ENOSPC\n", how, err, got >> 20); bad = 1; }
		if (got < (16L << 20)) { printf("FAIL: %s: ENOSPC after only %ld MB\n", how, got >> 20); bad = 1; }
		if (!still_alive()) { printf("FAIL: %s: fork+exec failed with the file full\n", how); bad = 1; }
		close(fd);
		if (mode != 2) unlink("/ramfsfull.dat");
		_exit(bad);
	}
	int st = 0;
	waitpid(c, &st, 0);
	if (WIFSIGNALED(st)) FAIL("%s: writer killed by signal %d", how, WTERMSIG(st));
	else if (WEXITSTATUS(st)) fails++;
	unlink("/ramfsfull.dat");
	long after = meminfo_kb("MemFree");
	if (before > 0 && after >= 0 && after < before - 32768)
		FAIL("%s: %ld KB free before, %ld KB after the file went", how, before, after);
	if (!still_alive()) FAIL("%s: fork+exec failed afterwards", how);
}

int main(void)
{
	one("write", 0);
	one("sendfile", 1);
	one("memfd", 2);
	printf(fails ? "FAILED %d\n" : "OK\n", fails);
	return fails != 0;
}
