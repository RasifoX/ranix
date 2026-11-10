/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Print Screen reaches programs as KEY_SYSRQ (99) both ways a PC keyboard sends it:
 * E0 37 alone, and 54 (SysRq, no E0) while Alt is held; before, the second came
 * as code 84. The desk takes Print Screen from the console (Alt: only the window).
 * Keys go in through /dev/input/rawkbd. First failure or OK. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

struct ev { int64_t s, us; uint16_t type, code; int32_t value; };

static void msleep(int ms)
{
	struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
	nanosleep(&t, NULL);
}

/* after the bytes, the key events: 99 down and up once, nothing else but Alt (56) */
static int sends(int kfd, int efd, const uint8_t *b, int n, const char *what)
{
	struct ev evs[64];
	while (read(efd, evs, sizeof evs) > 0)
		;
	if (write(kfd, b, (size_t)n) != n) { printf("FAIL write errno=%d\n", errno); return 1; }
	msleep(150);
	ssize_t got = read(efd, evs, sizeof evs);
	int down = 0, up = 0, other = 0;
	for (int j = 0; got > 0 && j < (int)(got / (ssize_t)sizeof evs[0]); j++) {
		if (evs[j].type != 1) continue;
		if (evs[j].code == 99) { if (evs[j].value) down++; else up++; }
		else if (evs[j].code != 56) other = evs[j].code;
	}
	if (down != 1 || up != 1 || other) {
		printf("FAIL %s: want 99 down/up, got down=%d up=%d other=%d\n", what, down, up, other);
		return 1;
	}
	return 0;
}

int main(void)
{
	int kfd = open("/dev/input/rawkbd", O_WRONLY);
	int efd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	if (kfd < 0 || efd < 0) { printf("FAIL open errno=%d\n", errno); return 1; }
	static const uint8_t plain[] = {0xE0, 0x37, 0xE0, 0xB7};
	static const uint8_t alt[] = {0x38, 0x54, 0xD4, 0xB8};
	if (sends(kfd, efd, plain, 4, "E0 37") || sends(kfd, efd, alt, 4, "Alt + 54"))
		return 1;
	close(kfd);
	close(efd);
	printf("OK\n");
	return 0;
}
