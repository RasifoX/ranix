/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Host test of the joystick and gamepad arithmetic:
 *   cc -I. tests/host_hidjoy.c drivers/input/hidjoy.c && ./a.out
 * Report descriptors in the shape of what FRC drivers plug in: a dual-stick pad
 * in DirectInput mode (Logitech F310 "D"), a gamepad with a report id (DualShock 4
 * style), a flight stick with 10-bit axes and split buttons (Extreme 3D style), a
 * pad with signed axes; and the Xbox 360 (XInput) report. */
#include <stdio.h>
#include <string.h>
#include "drivers/input/hidjoy.h"

static int fails, passes;
#define CHECK(name, cond) do { if (cond) passes++; else { fails++; printf("FAIL: %s\n", name); } } while (0)

static struct { uint16_t type, code; int32_t v; } ev[128];
static int nev;
static void emit(void *ctx, uint16_t type, uint16_t code, int32_t value)
{
	(void)ctx;
	if (nev < 128) { ev[nev].type = type; ev[nev].code = code; ev[nev].v = value; nev++; }
}
/* the last value reported for (type, code), or -999 */
static int32_t val(uint16_t type, uint16_t code)
{
	int32_t v = -999;
	for (int i = 0; i < nev; i++) if (ev[i].type == type && ev[i].code == code) v = ev[i].v;
	return v;
}
static int key(const hj_caps_t *c, int code) { return c->keybits[code / 64] >> (code % 64) & 1; }

static const uint8_t f310d[] = {
	0x05, 0x01, 0x09, 0x04, 0xA1, 0x01, 0xA1, 0x02,
	0x75, 0x08, 0x95, 0x04, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x35, 0x00, 0x46, 0xFF, 0x00,
	0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x81, 0x02,
	0x75, 0x04, 0x95, 0x01, 0x25, 0x07, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x09, 0x39, 0x81, 0x42,
	0x65, 0x00, 0x75, 0x01, 0x95, 0x0C, 0x25, 0x01, 0x45, 0x01,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x0C, 0x81, 0x02,
	0x06, 0x00, 0xFF, 0x75, 0x01, 0x95, 0x08, 0x25, 0x01, 0x09, 0x01, 0x81, 0x02,
	0xC0, 0xC0,
};
static const uint8_t ds4ish[] = {
	0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,
	0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26, 0xFF, 0x00,
	0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
	0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x0E, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0E, 0x81, 0x02,
	0x06, 0x00, 0xFF, 0x09, 0x20, 0x75, 0x06, 0x95, 0x01, 0x81, 0x02,
	0x05, 0x01, 0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
	0x06, 0x00, 0xFF, 0x85, 0x05, 0x09, 0x22, 0x95, 0x1F, 0x91, 0x02,   /* output report 5: not ours */
	0xC0,
};
static const uint8_t stick[] = {
	0x05, 0x01, 0x09, 0x04, 0xA1, 0x01, 0xA1, 0x02,
	0x15, 0x00, 0x26, 0xFF, 0x03, 0x75, 0x0A, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31, 0x81, 0x02,
	0x25, 0x07, 0x75, 0x04, 0x95, 0x01, 0x09, 0x39, 0x81, 0x42,
	0x75, 0x04, 0x95, 0x01, 0x81, 0x01,                                   /* padding */
	0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x09, 0x35, 0x81, 0x02,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
	0x05, 0x01, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x01, 0x09, 0x36, 0x81, 0x02,
	0x05, 0x09, 0x19, 0x09, 0x29, 0x0C, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
	0x75, 0x04, 0x95, 0x01, 0x81, 0x01,
	0xC0, 0xC0,
};
static const uint8_t signedpad[] = {
	0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
	0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x09, 0x30, 0x09, 0x31, 0x81, 0x02,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
	0x95, 0x06, 0x81, 0x01,
	0xC0,
};
static const uint8_t keyboard[] = {
	0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0xC0,
};
static const uint8_t mouse[] = {
	0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
	0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06,
	0xC0, 0xC0,
};

static void t_f310(void)
{
	hidjoy_t j;
	hj_caps_t c;
	CHECK("f310 is joystick", hj_parse(f310d, sizeof f310d, &j) == 1 && !j.gamepad && !j.ids);
	hj_caps(&j, &c);
	CHECK("f310 axes", (c.absbits & 0x3F) == ((1 << 0) | (1 << 1) | (1 << 2) | (1 << 5)) &&
	      c.absmin[0] == 0 && c.absmax[0] == 255);
	CHECK("f310 hat", (c.absbits >> 0x10 & 3) == 3);
	CHECK("f310 12 buttons", key(&c, 0x120) && key(&c, 0x12B) && !key(&c, 0x12C) && !key(&c, 0x130));
	/* X 0, Y 255, Z 128, Rz 7; hat 2 (east); buttons 1 and 12 */
	uint8_t r[7] = { 0, 255, 128, 7, 0x02 | 0x10, 0x80, 0xFF };
	nev = 0;
	hj_report(&j, r, sizeof r, emit, 0);
	CHECK("f310 x y", val(3, 0) == 0 && val(3, 1) == 255 && val(3, 2) == 128 && val(3, 5) == 7);
	CHECK("f310 hat east", val(3, 0x10) == 1 && val(3, 0x11) == 0);
	CHECK("f310 buttons", val(1, 0x120) == 1 && val(1, 0x121) == 0 && val(1, 0x12B) == 1);
	r[4] = 0x08;                     /* hat 8: outside 0..7, centred */
	nev = 0;
	hj_report(&j, r, sizeof r, emit, 0);
	CHECK("f310 hat centre", val(3, 0x10) == 0 && val(3, 0x11) == 0);
	r[4] = 0x07;                     /* north-west */
	nev = 0;
	hj_report(&j, r, sizeof r, emit, 0);
	CHECK("f310 hat nw", val(3, 0x10) == -1 && val(3, 0x11) == -1);
}

static void t_ds4(void)
{
	hidjoy_t j;
	hj_caps_t c;
	CHECK("ds4 is gamepad", hj_parse(ds4ish, sizeof ds4ish, &j) == 1 && j.gamepad && j.ids);
	hj_caps(&j, &c);
	CHECK("ds4 buttons", key(&c, 0x130) && key(&c, 0x13D) && !key(&c, 0x13E) && !key(&c, 0x120));
	CHECK("ds4 triggers", (c.absbits >> 3 & 3) == 3);
	uint8_t r[10] = { 1, 10, 20, 30, 40, 0x00 | 0x20, 0x00, 0x02, 200, 100 };   /* hat N, button 2, 14 */
	nev = 0;
	hj_report(&j, r, sizeof r, emit, 0);
	CHECK("ds4 axes", val(3, 0) == 10 && val(3, 1) == 20 && val(3, 5) == 40 && val(3, 3) == 200 && val(3, 4) == 100);
	CHECK("ds4 hat north", val(3, 0x11) == -1 && val(3, 0x10) == 0);
	CHECK("ds4 buttons", val(1, 0x131) == 1 && val(1, 0x130) == 0 && val(1, 0x13D) == 1);
	r[0] = 5;
	nev = 0;
	hj_report(&j, r, sizeof r, emit, 0);
	CHECK("ds4 other id ignored", nev == 0);
	nev = 0;
	hj_report(&j, r, 3, emit, 0);
	CHECK("ds4 short ignored", nev == 0);
}

static void t_stick(void)
{
	hidjoy_t j;
	hj_caps_t c;
	CHECK("stick", hj_parse(stick, sizeof stick, &j) == 1);
	hj_caps(&j, &c);
	CHECK("stick 10-bit", c.absmax[0] == 1023 && c.absmax[1] == 1023 && c.absmax[5] == 255 && c.absmax[6] == 255);
	CHECK("stick 12 buttons", key(&c, 0x120) && key(&c, 0x12B) && !key(&c, 0x12C));
	/* X 1023, Y 512; hat 4 (south); Rz 9; buttons 1, 8; slider 77; button 12 */
	uint8_t r[8];
	memset(r, 0, sizeof r);
	uint32_t x = 1023, y = 512;
	uint64_t b = x | (uint64_t)y << 10 | (uint64_t)4 << 20 | (uint64_t)9 << 28 | (uint64_t)0x81 << 36 |
	             (uint64_t)77 << 44 | (uint64_t)0x8 << 52;
	for (int i = 0; i < 8; i++) r[i] = (uint8_t)(b >> (8 * i));
	nev = 0;
	hj_report(&j, r, sizeof r, emit, 0);
	CHECK("stick axes", val(3, 0) == 1023 && val(3, 1) == 512 && val(3, 5) == 9 && val(3, 6) == 77);
	CHECK("stick hat south", val(3, 0x10) == 0 && val(3, 0x11) == 1);
	CHECK("stick buttons", val(1, 0x120) == 1 && val(1, 0x127) == 1 && val(1, 0x121) == 0 &&
	      val(1, 0x128) == 0 && val(1, 0x12B) == 1);
}

static void t_other(void)
{
	hidjoy_t j;
	CHECK("signed pad", hj_parse(signedpad, sizeof signedpad, &j) == 1);
	uint8_t r[3] = { 0x81, 0x7F, 0x01 };
	nev = 0;
	hj_report(&j, r, sizeof r, emit, 0);
	CHECK("signed values", val(3, 0) == -127 && val(3, 1) == 127 && val(1, 0x130) == 1);
	CHECK("keyboard no", hj_parse(keyboard, sizeof keyboard, &j) == 0);
	CHECK("mouse no", hj_parse(mouse, sizeof mouse, &j) == 0);
	int bad = 0;
	for (uint32_t n = 0; n < sizeof stick; n++) hj_parse(stick, n, &j);   /* must not crash */
	uint8_t junk[200];
	for (int i = 0; i < 200; i++) junk[i] = (uint8_t)(i * 89 + 7);
	for (int k = 0; k < 50; k++) { junk[k] ^= 0x5A; if (hj_parse(junk, sizeof junk, &j) < 0) bad++; }
	CHECK("junk safe", bad == 0);
}

static void t_xpad(void)
{
	hj_caps_t c;
	hj_xpad_caps(&c);
	CHECK("xpad caps", key(&c, 0x130) && key(&c, 0x134) && key(&c, 0x13C) && (c.absbits & 0x3F) == 0x3F &&
	      c.absmin[0] == -32768 && c.absmax[2] == 255);
	/* dpad up + left, start; A and LB; LT 255, RT 0; LX -32768, LY 32767 (up), RX 1, RY -1 */
	uint8_t r[20] = { 0x00, 0x14, 0x01 | 0x04 | 0x10, 0x10 | 0x01, 255, 0,
	                  0x00, 0x80, 0xFF, 0x7F, 0x01, 0x00, 0xFF, 0xFF };
	nev = 0;
	hj_xpad_report(r, sizeof r, emit, 0);
	CHECK("xpad buttons", val(1, 0x130) == 1 && val(1, 0x136) == 1 && val(1, 0x13B) == 1 && val(1, 0x131) == 0);
	CHECK("xpad dpad", val(3, 0x10) == -1 && val(3, 0x11) == -1);
	CHECK("xpad sticks", val(3, 0) == -32768 && val(3, 1) == -32768 && val(3, 3) == 1 && val(3, 4) == 0);
	CHECK("xpad triggers", val(3, 2) == 255 && val(3, 5) == 0);
	uint8_t led[3] = { 0x01, 0x03, 0x06 };
	nev = 0;
	hj_xpad_report(led, sizeof led, emit, 0);
	CHECK("xpad other message", nev == 0);
}

static void t_gip(void)
{
	hj_caps_t c;
	hj_gip_caps(&c);
	CHECK("gip caps", key(&c, 0x130) && key(&c, 0x13C) && c.absmax[2] == 1023 && c.absmin[0] == -32768);
	uint8_t b[16];
	CHECK("gip power on", hj_gip_init(b, 0, 7) == 5 && b[0] == 0x05 && b[1] == 0x20 && b[2] == 7 && b[3] == 0x01);
	CHECK("gip led", hj_gip_init(b, 1, 8) == 7 && b[0] == 0x0A && b[2] == 8);
	CHECK("gip end", hj_gip_init(b, 2, 9) == 0);
	/* A + menu; dpad down + right, RB; LT 1023, RT 512; LX 32767, LY -32768, RX -1, RY 0 */
	uint8_t r[18] = { 0x20, 0x00, 0x05, 0x0E, 0x10 | 0x04, 0x02 | 0x08 | 0x20,
	                  0xFF, 0x03, 0x00, 0x02, 0xFF, 0x7F, 0x00, 0x80, 0xFF, 0xFF, 0, 0 };
	nev = 0;
	hj_gip_report(r, sizeof r, emit, 0);
	CHECK("gip buttons", val(1, 0x130) == 1 && val(1, 0x13b) == 1 && val(1, 0x137) == 1 && val(1, 0x131) == 0);
	CHECK("gip dpad", val(3, 0x10) == 1 && val(3, 0x11) == 1);
	CHECK("gip triggers", val(3, 2) == 1023 && val(3, 5) == 512);
	CHECK("gip sticks", val(3, 0) == 32767 && val(3, 1) == 32767 && val(3, 3) == -1 && val(3, 4) == -1);
	uint8_t g[6] = { 0x07, 0x20, 0x01, 0x02, 0x01, 0x5B };
	nev = 0;
	hj_gip_report(g, sizeof g, emit, 0);
	CHECK("gip guide", nev == 1 && val(1, 0x13c) == 1);
	uint8_t hello[4] = { 0x02, 0x20, 0x01, 0x1C };
	nev = 0;
	hj_gip_report(hello, sizeof hello, emit, 0);
	CHECK("gip announce ignored", nev == 0);
}

int main(void)
{
	t_gip();
	t_f310();
	t_ds4();
	t_stick();
	t_other();
	t_xpad();
	printf("hidjoy: %d passed, %d failed\n", passes, fails);
	if (!fails) printf("ALL PASS\n");
	return fails != 0;
}
