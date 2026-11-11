/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include "synaptics.h"

#define Z_DOWN    30      /* pressure of a landing finger */
#define Z_UP      18      /* and of one still resting: a light stroke must not break up */
#define W_PALM    12      /* a width this large is a palm, not a finger */
#define UNIT      2       /* pad units per count: about 20 counts/mm, smooth at any speed */
#define SETTLE    2       /* packets after a landing whose position is still moving */
#define JITTER    2       /* pad units of noise on a finger at rest */
#define TAP_MS    220
#define DRAG_MS   250     /* second touch this soon after a tap carries the button */
#define TAP_MOVE  120     /* pad units; more is a stroke, not a tap */
#define SCROLL    70      /* pad units per wheel step */
#define CLICK_MS  90      /* a finger rocks as the pad is pressed: hold the pointer */
#define BTN_LEFT  1
#define BTN_RIGHT 2
#define BTN_MIDDLE 4
#define LOCK_AT   24      /* pad units of two-finger travel before the axis is picked */
/* Lower right corner of a pad that is one big button */
#define RZONE_X   4200
#define RZONE_Y   2100

int syn_tap = 1;          /* a touch is a click: the laptop default, off in Settings */
int syn_tap3 = 1;         /* three fingers: the middle button, as libinput does; off: the right */
int syn_middle = 1;       /* left and right pressed together: the middle button */

int syn_option(const char *k, unsigned long n)
{
	static const struct { const char *name; int *v; } O[] = {{"tap ", &syn_tap}, {"tap3 ", &syn_tap3},
	                                                         {"middle ", &syn_middle}};
	for (unsigned i = 0; i < sizeof O / sizeof O[0]; i++) {
		unsigned long l = 0;
		while (O[i].name[l]) l++;
		if (n < l + 1 || (k[l] != '0' && k[l] != '1')) continue;
		unsigned long j = 0;
		while (j < l && k[j] == O[i].name[j]) j++;
		if (j < l) continue;
		*O[i].v = k[l] == '1';
		return 0;
	}
	return -1;
}

int syn_options(char *out, int cap)
{
	const int v[3] = {syn_tap, syn_tap3, syn_middle};
	static const char *const K[3] = {"tap ", " tap3 ", " middle "};
	int n = 0;
	for (int i = 0; i < 3; i++) {
		for (const char *k = K[i]; *k && n < cap - 3; k++) out[n++] = *k;
		if (n < cap - 2) out[n++] = (char)('0' + (v[i] != 0));
	}
	out[n++] = '\n';
	return n;
}

/* Two fingers: the page moves with them, a pixel at a time, both ways. The axis
 * the fingers mostly travel along is picked once and kept, so a vertical scroll
 * does not drift sideways. Wheel notches still go out for readers that know only
 * those (s->sy). */
static void two_fingers(syn_t *s, int dx, int dy, int btn, syn_emit_t emit)
{
	s->sy += dy;
	int steps = s->sy / SCROLL;
	s->sy -= steps * SCROLL;
	if (steps) emit(0, 0, steps, btn);
	if (!s->scroll) return;
	if (!s->lock) {
		s->ax += dx < 0 ? -dx : dx;
		s->ay += dy < 0 ? -dy : dy;
		if (s->ax + s->ay < LOCK_AT) {           /* not yet: kept for when it is */
			s->fx += dx * 120;
			s->fy += dy * 120;
			return;
		}
		s->lock = s->ay >= 2 * s->ax ? 1 : s->ax >= 2 * s->ay ? 2 : 3;
	}
	if (s->lock != 2) s->fy += dy * 120;
	else s->fy = 0;
	if (s->lock != 1) s->fx += dx * 120;
	else s->fx = 0;
	int v = s->fy / SCROLL, h = s->fx / SCROLL;
	s->fy -= v * SCROLL;
	s->fx -= h * SCROLL;
	if (v || h) {
		s->scroll(v, h, 0);
		s->scrolled = 1;
	}
}

static void scroll_end(syn_t *s)
{
	if (s->scrolled && s->scroll) s->scroll(0, 0, 1);
	s->scrolled = s->lock = s->ax = s->ay = s->fx = s->fy = 0;
}

void syn_packet(syn_t *s, const uint8_t p[6], uint64_t ms, syn_emit_t emit)
{
	int w = ((p[3] >> 2) & 1) | ((p[0] >> 1) & 2) | ((p[0] >> 2) & 4) | ((p[0] >> 2) & 8);
	if (w == 2) return;   /* extended packet: a second finger's position, not used */
	int x = ((p[3] & 0x10) << 8) | ((p[1] & 0x0F) << 8) | p[4];
	int y = ((p[3] & 0x20) << 7) | ((p[1] & 0xF0) << 4) | p[5];
	int z = p[2];
	int click = (p[0] & 1) | ((p[0] ^ p[3]) & 1);
	int rclick = (p[0] >> 1) & 1;
	int fingers = w == 0 ? 2 : w == 1 ? 3 : 1;

	int btn = 0;
	if (click) {
		int many = fingers > s->maxf ? fingers : s->maxf;
		if (s->down && many >= 3) btn = syn_tap3 ? BTN_MIDDLE : BTN_RIGHT;
		else if (s->down && many >= 2) btn = BTN_RIGHT;
		else btn = (x > RZONE_X && y < RZONE_Y && s->down) ? BTN_RIGHT : BTN_LEFT;
	}
	if (rclick) btn |= BTN_RIGHT;
	if (syn_middle && (p[0] & 3) == 3 && ((p[0] ^ p[3]) & 3) == 0) btn = BTN_MIDDLE;   /* both buttons */
	if (btn && !s->clicked) s->click_t = ms;
	if (btn) s->clicked = 1;
	if (s->dragging) btn |= BTN_LEFT;

	if (z < (s->down ? Z_UP : Z_DOWN)) {
		scroll_end(s);
		if (s->down) {
			int taps = syn_tap && !s->clicked && !s->dragging && ms - s->t0 <= TAP_MS && s->moved < TAP_MOVE;
			s->down = 0;
			if (s->dragging) {
				s->dragging = 0;
				btn &= ~BTN_LEFT;
			}
			if (taps) {
				int b = s->maxf >= 3 ? (syn_tap3 ? BTN_MIDDLE : BTN_RIGHT) : s->maxf >= 2 ? BTN_RIGHT : BTN_LEFT;
				emit(0, 0, 0, b);
				emit(0, 0, 0, 0);
				s->buttons = 0;
				s->tap_t = ms;
				s->tapped = b == BTN_LEFT;
			}
		}
		if (btn != s->buttons) { s->buttons = btn; emit(0, 0, 0, btn); }
		return;
	}
	if (fingers == 1 && w >= W_PALM) {       /* a resting palm moves nothing */
		s->lx = x; s->ly = y;
		s->settle = SETTLE;
		return;
	}
	if (!s->down) {
		s->down = 1;
		s->t0 = ms;
		s->moved = s->clicked = 0;
		s->maxf = s->fingers = fingers;
		s->lx = x; s->ly = y;
		s->rx = s->ry = s->sy = s->px = s->py = 0;
		s->settle = SETTLE;
		s->moving = 0;
		s->dragging = s->tapped && ms - s->tap_t <= DRAG_MS && fingers == 1;
		s->tapped = 0;
		if (s->dragging) btn |= BTN_LEFT;
		if (btn) s->clicked = 1;
		if (btn != s->buttons) { s->buttons = btn; emit(0, 0, 0, btn); }
		return;
	}
	if (fingers > s->maxf) s->maxf = fingers;
	int dx = x - s->lx, dy = y - s->ly;
	if (fingers != s->fingers) {             /* the reported point jumps when one lands */
		s->settle = SETTLE;
		s->px = s->py = 0;
	}
	s->fingers = fingers;
	s->lx = x; s->ly = y;
	if (s->settle) {
		s->settle--;
		if (btn != s->buttons) { s->buttons = btn; emit(0, 0, 0, btn); }
		return;
	}
	s->moved += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);

	if (fingers >= 2 && !btn) {
		two_fingers(s, dx, dy, btn, emit);
		return;
	}
	if (s->scrolled) scroll_end(s);           /* one finger left: the scroll is over */
	if (s->clicked && ms - s->click_t < CLICK_MS) dx = dy = 0;
	/* Noise on a resting finger stays put; once it travels, nothing is thrown away */
	if (!s->moving) {
		int a = dx < 0 ? -dx : dx, b = dy < 0 ? -dy : dy;
		if (a <= JITTER && b <= JITTER) dx = dy = 0;
		else s->moving = 1;
	}
	/* Two packets averaged: the sensor's step noise goes, the delay is 6 ms */
	int sx = dx + s->px, sy = dy + s->py;
	s->px = dx; s->py = dy;
	s->rx += sx;
	s->ry += sy;
	int ox = s->rx / (2 * UNIT), oy = s->ry / (2 * UNIT);
	s->rx -= ox * 2 * UNIT;
	s->ry -= oy * 2 * UNIT;
	if (ox || oy || btn != s->buttons) {
		s->buttons = btn;
		emit(ox, oy, 0, btn);
	}
}
