/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* madvise(DONTNEED / FREE) over a huge, mostly empty reservation, the way V8
 * drops its cage: 256 GB PROT_NONE with a few pages made writable and touched.
 * The call must cost the populated part, not every 4 KB of the range (a page
 * at a time it held a CPU in the kernel for 10 s), and the touched pages must
 * read back as zero after DONTNEED (after FREE Linux may keep them until reclaim).
 * Prints OK, or FAIL with what was wrong. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define GB  (1UL << 30)
#define BIG (256 * GB)

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static const char *one(int advice, const char *name)
{
	static char why[96];
	char *p = mmap(NULL, BIG, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (p == MAP_FAILED) { snprintf(why, sizeof why, "mmap 256 GB: errno %d", errno); return why; }
	const unsigned long at[] = { 0, 4096, 37 * GB, 128 * GB + 8192, BIG - 4096 };
	for (unsigned i = 0; i < 5; i++) {
		if (mprotect(p + at[i], 4096, PROT_READ | PROT_WRITE)) return "mprotect";
		memset(p + at[i], 0x5a, 4096);
	}
	double t0 = now();
	int r = madvise(p, BIG, advice);
	double dt = now() - t0;
	if (r) { snprintf(why, sizeof why, "%s: errno %d", name, errno); return why; }
	if (dt > 1.0) { snprintf(why, sizeof why, "%s over 256 GB took %.1f s", name, dt); return why; }
	for (unsigned i = 0; advice == MADV_DONTNEED && i < 5; i++)
		if (p[at[i]] != 0 || p[at[i] + 4095] != 0) {
			snprintf(why, sizeof why, "%s: page at %#lx kept its data", name, at[i]);
			return why;
		}
	munmap(p, BIG);
	return NULL;
}

int main(void)
{
	const char *e = one(MADV_DONTNEED, "DONTNEED");
	if (!e) e = one(MADV_FREE, "FREE");
	if (e) { printf("FAIL: %s\n", e); return 1; }
	printf("OK\n");
	return 0;
}
