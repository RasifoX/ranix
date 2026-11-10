/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A laptop's sound keys (set 1: E0 20 mute, E0 2E down, E0 30 up) reach programs
 * as KEY_MUTE, KEY_VOLUMEDOWN and KEY_VOLUMEUP, not as the letters that share
 * their low byte ('d', 'c', 'b'): a browser with the keyboard typed a letter at
 * every press. The play keys likewise. Keys go in through /dev/input/rawkbd. */
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

int main(void)
{
	static const struct { uint8_t sc; uint16_t want; } K[] = {
		{0x20, 113}, {0x2E, 114}, {0x30, 115}, {0x22, 164}, {0x19, 163}, {0x10, 165},
	};
	int kfd = open("/dev/input/rawkbd", O_WRONLY);
	int efd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
	if (kfd < 0 || efd < 0) { printf("FAIL open errno=%d\n", errno); return 1; }
	struct ev evs[64];
	while (read(efd, evs, sizeof evs) > 0)
		;
	int vfd = open("/sys/kernel/volume", O_RDWR), vol = -1;
	char vb[16];
	if (vfd >= 0) {                          /* the press itself changes the volume: put it back */
		ssize_t n = read(vfd, vb, sizeof vb - 1);
		vb[n > 0 ? n : 0] = 0;
		if (n > 0) sscanf(vb, "%d", &vol);
	}
	int bad = 0;
	for (unsigned i = 0; i < sizeof K / sizeof K[0]; i++) {
		uint8_t b[4] = {0xE0, K[i].sc, 0xE0, (uint8_t)(K[i].sc | 0x80)};
		if (write(kfd, b, 4) != 4) { printf("FAIL write errno=%d\n", errno); return 1; }
		msleep(120);
		ssize_t got = read(efd, evs, sizeof evs);
		int down = 0, up = 0, other = 0;
		for (int j = 0; got > 0 && j < (int)(got / (ssize_t)sizeof evs[0]); j++) {
			if (evs[j].type != 1) continue;
			if (evs[j].code == K[i].want) { if (evs[j].value) down++; else up++; }
			else other = evs[j].code;
		}
		if (down != 1 || up != 1 || other) {
			printf("e0 %02x: want code %u down/up, got down=%d up=%d other=%d\n", K[i].sc, K[i].want, down, up, other);
			bad++;
		}
	}
	if (vfd >= 0 && vol >= 0) {
		int n = snprintf(vb, sizeof vb, "%d", vol);
		if (pwrite(vfd, vb, (size_t)n, 0) < 0 && write(vfd, vb, (size_t)n) < 0) { /* read-only for us */ }
		close(vfd);
	}
	close(kfd);
	close(efd);
	if (bad) { printf("FAIL %d keys\n", bad); return 1; }
	printf("OK\n");
	return 0;
}
