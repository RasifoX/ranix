/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /proc files LTP's library reads before a test starts: kernel/tainted (0 on a
 * kernel nothing tainted) and /proc/self/mounts (the same table as
 * /proc/mounts; tst_cgroup and others open it). Without them a test is broken
 * before it runs. */
#include <stdio.h>
#include <string.h>

static int bad;

static void first_line(const char *path, char *out, int n)
{
	FILE *f = fopen(path, "r");
	out[0] = '\0';
	if (!f) { printf("%s: cannot open\n", path); bad++; return; }
	if (!fgets(out, n, f)) { printf("%s: empty\n", path); bad++; }
	fclose(f);
}

int main(void)
{
	char a[256], b[256];
	first_line("/proc/sys/kernel/tainted", a, sizeof a);
	if (strcmp(a, "0\n") != 0 && a[0]) { printf("tainted: '%s'\n", a); bad++; }
	first_line("/proc/self/mounts", a, sizeof a);
	first_line("/proc/mounts", b, sizeof b);
	if (strcmp(a, b) != 0) { printf("self/mounts differs: '%s' vs '%s'\n", a, b); bad++; }
	puts(bad ? "FAIL" : "OK");
	return bad != 0;
}
