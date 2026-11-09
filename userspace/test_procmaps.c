/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /proc/self/maps and status as Linux writes them (LTP mmap04..mmap20,
 * mlock201/203, munlockall01, getrusage03 parse them): a range starts with at
 * least 8 hex digits, not 16 ("%08lx"), a MAP_SHARED mapping says 's', and
 * status has VmLck (locked kB) and VmSwap (0, no swap). */
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int bad;

static long status_kb(const char *key)
{
	char line[256]; long v = -1;
	FILE *f = fopen("/proc/self/status", "r");
	while (f && fgets(line, sizeof line, f))
		if (!strncmp(line, key, strlen(key))) sscanf(line + strlen(key), " %ld", &v);
	if (f) fclose(f);
	return v;
}

static int maps_perms(void *addr, char *perms)
{
	char line[512], want[32];
	snprintf(want, sizeof want, "%08" PRIxPTR "-", (uintptr_t)addr);
	FILE *f = fopen("/proc/self/maps", "r");
	int found = 0;
	while (f && fgets(line, sizeof line, f))
		if (!strncmp(line, want, strlen(want))) { sscanf(line, "%*x-%*x %7s", perms); found = 1; }
	if (f) fclose(f);
	return found;
}

int main(void)
{
	long pg = sysconf(_SC_PAGESIZE);
	char perms[8] = "";
	char *s = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	char *p = mmap(NULL, pg, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (!maps_perms(s, perms)) { printf("maps: no line starting %08" PRIxPTR "-\n", (uintptr_t)s); bad++; }
	else if (strcmp(perms, "rw-s")) { printf("maps: shared perms '%s', want rw-s\n", perms); bad++; }
	if (maps_perms(p, perms) && strcmp(perms, "r--p")) { printf("maps: private perms '%s', want r--p\n", perms); bad++; }

	long l0 = status_kb("VmLck:"), sw = status_kb("VmSwap:");
	if (l0 != 0) { printf("VmLck before mlock: %ld, want 0\n", l0); bad++; }
	if (sw != 0) { printf("VmSwap: %ld, want 0\n", sw); bad++; }
	mlock(s, 2 * pg);
	long l1 = status_kb("VmLck:");
	if (l1 != 2 * pg / 1024) { printf("VmLck after mlock: %ld, want %ld\n", l1, 2 * pg / 1024); bad++; }
	munlock(s, 2 * pg);
	if (status_kb("VmLck:") != 0) { puts("VmLck after munlock not 0"); bad++; }
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
