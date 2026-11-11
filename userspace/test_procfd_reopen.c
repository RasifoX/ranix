/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* open("/proc/self/fd/N") of a /proc file makes a second open file description
 * of the same in-memory inode. The first close used to free that inode while
 * the second still held it, and the second close freed it again: the heap's
 * "kfree bad ptr" (found by test_fuzz in the full suite). Both descriptions
 * must keep working until their own close, and the heap must stay quiet.
 * Prints FAIL lines, then OK or FAIL. Runs as root (reads the kernel log). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/klog.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int bad_frees(void)
{
	int n = klogctl(10, NULL, 0);            /* SYSLOG_ACTION_SIZE_BUFFER */
	if (n <= 0) n = 1 << 18;
	char *b = malloc((size_t)n + 1);
	if (!b) return -1;
	int len = klogctl(3, b, n);              /* SYSLOG_ACTION_READ_ALL */
	int c = 0;
	if (len > 0) {
		b[len] = 0;
		for (char *p = b; (p = strstr(p, "kfree bad ptr")); p++) c++;
	}
	free(b);
	return c;
}

int main(void)
{
	const char *files[] = { "/proc/self/status", "/proc/meminfo", "/proc/version", "/sys/kernel/ostype" };
	int before = bad_frees();
	CHECK(before >= 0, "cannot read the kernel log");
	for (int r = 0; r < 20; r++) {
		for (unsigned i = 0; i < sizeof files / sizeof files[0]; i++) {
			int a = open(files[i], O_RDONLY);
			if (a < 0) continue;
			char path[64], buf[64];
			snprintf(path, sizeof path, "/proc/self/fd/%d", a);
			int b = open(path, O_RDONLY);
			CHECK(b >= 0, "reopen %s through %s", files[i], path);
			if (b < 0) { close(a); continue; }
			close(a);                            /* the first goes, the second stays usable */
			CHECK(read(b, buf, sizeof buf) > 0, "read %s after the first close", files[i]);
			close(b);
		}
	}
	int after = bad_frees();
	CHECK(after == before, "%d bad frees in the kernel log", after - before);
	puts(fails ? "FAIL" : "OK");
	return fails != 0;
}
