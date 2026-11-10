/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Host test of the touchpad logic: cc -I. tests/host_synaptics.c drivers/input/ps2/synaptics.c */
#include <stdio.h>
#include <string.h>
#include "drivers/input/ps2/synaptics.h"

static int mx, my, wheel, presses, rpresses, mpresses, last, sv, sh, stops;

static void emit(int dx, int dy, int w, int b)
{
	mx += dx; my += dy; wheel += w;
	if ((b & 1) && !(last & 1)) presses++;
	if ((b & 2) && !(last & 2)) rpresses++;
	if ((b & 4) && !(last & 4)) mpresses++;
	last = b;
}

static void scroll(int v, int h, int stop) { sv += v; sh += h; stops += stop; }

static void pk(syn_t *s, int x, int y, int z, int w, int btn, uint64_t ms)
{
	uint8_t p[6];
	p[0] = 0x80 | ((w & 8) << 2) | ((w & 4) << 2) | ((w & 2) << 1) | (btn & 3);
	p[1] = ((y >> 4) & 0xF0) | ((x >> 8) & 0x0F);
	p[2] = (uint8_t)z;
	p[3] = 0xC0 | ((x >> 8) & 0x10) | ((y >> 7) & 0x20) | ((w & 1) << 2) | (btn & 3);
	p[4] = (uint8_t)x;
	p[5] = (uint8_t)y;
	syn_packet(s, p, ms, emit);
}

#define WANT(c) do { if (!(c)) { printf("FAIL line %d mx=%d my=%d wheel=%d p=%d r=%d\n", __LINE__, mx, my, wheel, presses, rpresses); return 1; } } while (0)
#define RESET() do { memset(&s, 0, sizeof s); s.scroll = scroll; \
	mx = my = wheel = presses = rpresses = mpresses = last = sv = sh = stops = 0; } while (0)

int main(void)
{
	syn_t s;
	RESET();                      /* a stroke moves and does not click */
	for (int i = 0; i < 40; i++) pk(&s, 2000 + i * 20, 3000, 60, 4, 0, i * 12);
	pk(&s, 2800, 3000, 0, 0, 0, 500);
	WANT(mx > 360 && mx < 400 && my == 0 && presses == 0);

	RESET();                      /* a short touch is a click, once */
	pk(&s, 3000, 3000, 50, 4, 0, 0); pk(&s, 3005, 3000, 50, 4, 0, 40); pk(&s, 3005, 3000, 0, 0, 0, 90);
	WANT(presses == 1 && rpresses == 0 && last == 0);

	RESET();                      /* a long touch is not */
	pk(&s, 3000, 3000, 50, 4, 0, 0); pk(&s, 3000, 3000, 50, 4, 0, 400); pk(&s, 3000, 3000, 0, 0, 0, 500);
	WANT(presses == 0);

	RESET();                      /* two fingers tapped: the right button */
	pk(&s, 3000, 3000, 50, 0, 0, 0); pk(&s, 3000, 3000, 50, 0, 0, 40); pk(&s, 3000, 3000, 0, 0, 0, 90);
	WANT(rpresses == 1 && presses == 0);

	RESET();                      /* two fingers moved: scrolling, no pointer motion */
	for (int i = 0; i < 30; i++) pk(&s, 3000, 3000 + i * 10, 60, 0, 0, i * 12);
	pk(&s, 3000, 3300, 0, 0, 0, 600);
	WANT(wheel >= 3 && wheel <= 4 && mx == 0 && my == 0 && rpresses == 0);

	RESET();                      /* a palm does nothing */
	for (int i = 0; i < 20; i++) pk(&s, 2000 + i * 30, 3000, 90, 14, 0, i * 12);
	WANT(mx == 0);

	RESET();                      /* pressing the pad clicks, lower right is the right button */
	pk(&s, 3000, 3000, 60, 4, 0, 0); pk(&s, 3000, 3000, 60, 4, 1, 50); pk(&s, 3000, 3000, 60, 4, 0, 120);
	WANT(presses == 1);
	pk(&s, 3000, 3000, 0, 0, 0, 300);
	RESET();
	pk(&s, 4800, 1800, 60, 4, 0, 0); pk(&s, 4800, 1800, 60, 4, 1, 50); pk(&s, 4800, 1800, 60, 4, 0, 120);
	WANT(rpresses == 1 && presses == 0);
	RESET();                      /* a resting finger's noise does not move the pointer */
	for (int i = 0; i < 60; i++) pk(&s, 3000 + (i & 1), 3000 - (i & 1), 60, 4, 0, i * 12);
	WANT(mx == 0 && my == 0);

	RESET();                      /* a landing's first readings are not motion */
	pk(&s, 3000, 3000, 40, 4, 0, 0); pk(&s, 3060, 3040, 50, 4, 0, 12); pk(&s, 3070, 3050, 60, 4, 0, 24);
	WANT(mx == 0 && my == 0);

	RESET();                      /* a light stroke survives a dip in pressure */
	for (int i = 0; i < 20; i++) pk(&s, 2000 + i * 20, 3000, i == 10 ? 22 : 40, 4, 0, i * 12);
	WANT(mx > 80 && s.down);

	RESET();                      /* tap, then touch and hold: the button goes with it */
	pk(&s, 3000, 3000, 50, 4, 0, 0); pk(&s, 3000, 3000, 0, 0, 0, 60);
	WANT(presses == 1 && last == 0);
	pk(&s, 3000, 3000, 50, 4, 0, 150);
	WANT(presses == 2 && (last & 1));
	for (int i = 0; i < 10; i++) pk(&s, 3000 + i * 30, 3000, 50, 4, 0, 170 + i * 12);
	pk(&s, 3300, 3000, 0, 0, 0, 400);
	WANT(mx > 0 && last == 0);

	RESET();                      /* two fingers pressing the pad: right button */
	pk(&s, 3000, 3000, 60, 0, 0, 0); pk(&s, 3000, 3000, 60, 0, 1, 50); pk(&s, 3000, 3000, 60, 0, 0, 120);
	WANT(rpresses == 1 && presses == 0);
	RESET();                      /* with tapping off a touch stays a touch */
	syn_tap = 0;
	pk(&s, 3000, 3000, 50, 4, 0, 0); pk(&s, 3005, 3000, 50, 4, 0, 40); pk(&s, 3005, 3000, 0, 0, 0, 90);
	syn_tap = 1;
	WANT(presses == 0);

	RESET();                      /* two fingers up: the page goes as they go, a notch is 120 */
	for (int i = 0; i < 31; i++) pk(&s, 3000, 3000 + i * 10, 60, 0, 0, i * 12);
	pk(&s, 3000, 3300, 0, 0, 0, 600);
	WANT(sv > 450 && sv < 520 && sh == 0 && stops == 1 && mx == 0 && my == 0);
	WANT(wheel >= 3 && wheel <= 4);              /* the notches still go, for old readers */

	RESET();                      /* two fingers sideways: horizontal, no notches */
	for (int i = 0; i < 31; i++) pk(&s, 3000 + i * 10, 3000, 60, 0, 0, i * 12);
	pk(&s, 3300, 3000, 0, 0, 0, 600);
	WANT(sh > 450 && sh < 520 && sv == 0 && wheel == 0 && stops == 1);

	RESET();                      /* down with a little drift: down only */
	for (int i = 0; i < 31; i++) pk(&s, 3000 + i * 2, 3000 - i * 10, 60, 0, 0, i * 12);
	pk(&s, 3060, 2700, 0, 0, 0, 600);
	WANT(sv < -450 && sh == 0);

	RESET();                      /* slow: every bit of travel arrives, nothing lost to rounding */
	for (int i = 0; i < 141; i++) pk(&s, 3000, 3000 + i, 60, 0, 0, i * 12);
	pk(&s, 3000, 3140, 0, 0, 0, 2000);
	WANT(sv >= 230 && sv <= 240);

	RESET();                      /* three fingers tapped: the middle button */
	pk(&s, 3000, 3000, 50, 1, 0, 0); pk(&s, 3000, 3000, 50, 1, 0, 40); pk(&s, 3000, 3000, 0, 0, 0, 90);
	WANT(mpresses == 1 && rpresses == 0 && presses == 0 && last == 0);
	RESET();                      /* three fingers pressing the pad: middle */
	pk(&s, 3000, 3000, 60, 1, 0, 0); pk(&s, 3000, 3000, 60, 1, 1, 50); pk(&s, 3000, 3000, 60, 1, 0, 120);
	WANT(mpresses == 1 && rpresses == 0 && presses == 0);
	RESET();                      /* the setting off: three fingers are the right button again */
	WANT(syn_option("tap3 0", 6) == 0 && syn_tap3 == 0);
	pk(&s, 3000, 3000, 50, 1, 0, 0); pk(&s, 3000, 3000, 50, 1, 0, 40); pk(&s, 3000, 3000, 0, 0, 0, 90);
	WANT(rpresses == 1 && mpresses == 0);
	WANT(syn_option("tap3 1", 6) == 0 && syn_tap3 == 1);

	RESET();                      /* both physical buttons at once: middle */
	pk(&s, 3000, 3000, 60, 4, 3, 0); pk(&s, 3000, 3000, 60, 4, 0, 60);
	WANT(mpresses == 1 && presses == 0 && rpresses == 0);
	WANT(syn_option("middle 0", 8) == 0 && syn_middle == 0);
	RESET();
	pk(&s, 3000, 3000, 60, 4, 3, 0); pk(&s, 3000, 3000, 60, 4, 0, 60);
	WANT(mpresses == 0 && presses == 1 && rpresses == 1);
	WANT(syn_option("middle 1", 8) == 0);
	WANT(syn_option("tap 0", 5) == 0 && syn_tap == 0 && syn_option("tap 1\n", 6) == 0 && syn_tap == 1);
	WANT(syn_option("tap 2", 5) == -1 && syn_option("bogus 1", 7) == -1 && syn_option("tap", 3) == -1);
	puts("OK");
	return 0;
}
