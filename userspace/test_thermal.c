/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /sys/kernel/ranix_thermal: the package thermal guard reports either "off (...)"
 * (QEMU, non-Intel) or its state with every field present and sane. Prints OK. */
#include <stdio.h>
#include <string.h>

int main(void)
{
	char b[512] = {0};
	FILE *f = fopen("/sys/kernel/ranix_thermal", "r");
	if (!f) { puts("FAIL open"); return 1; }
	size_t n = fread(b, 1, sizeof(b) - 1, f);
	fclose(f);
	if (!n || b[n - 1] != '\n') { puts("FAIL format"); return 1; }
	if (!strncmp(b, "off (", 5)) { puts("OK"); return 0; }
	const char *keys[] = { "temp_C", "max_C", "tjmax_C", "limit_mW", "firmware_mW", "steps", "emergencies", "restores", "locked" };
	unsigned v[9];
	for (int i = 0; i < 9; i++) {
		char *p = strstr(b, keys[i]);
		if (!p || sscanf(p + strlen(keys[i]), " %u", &v[i]) != 1) { printf("FAIL %s\n", keys[i]); return 1; }
	}
	if (v[2] < 70 || v[2] > 125 || v[0] > v[2] || v[1] < v[0] || v[8] > 1) { puts("FAIL values"); return 1; }
	puts("OK");
	return 0;
}
