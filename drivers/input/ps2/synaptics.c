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
/* Lower right corner of a pad that is one big button */
#define RZONE_X   4200
#define RZONE_Y   2100

int syn_tap = 1;          /* a touch is a click: the laptop default, off in Settings */

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
		if (s->down && (fingers >= 2 || s->maxf >= 2)) btn = BTN_RIGHT;
		else btn = (x > RZONE_X && y < RZONE_Y && s->down) ? BTN_RIGHT : BTN_LEFT;
	}
	if (rclick) btn |= BTN_RIGHT;
	if (btn && !s->clicked) s->click_t = ms;
	if (btn) s->clicked = 1;
	if (s->dragging) btn |= BTN_LEFT;

	if (z < (s->down ? Z_UP : Z_DOWN)) {
		if (s->down) {
			int taps = syn_tap && !s->clicked && !s->dragging && ms - s->t0 <= TAP_MS && s->moved < TAP_MOVE;
			s->down = 0;
			if (s->dragging) {
				s->dragging = 0;
				btn &= ~BTN_LEFT;
			}
			if (taps) {
				int b = s->maxf >= 2 ? BTN_RIGHT : BTN_LEFT;
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
		s->sy += dy;
		int steps = s->sy / SCROLL;
		s->sy -= steps * SCROLL;
		if (steps) emit(0, 0, steps, btn);
		return;
	}
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
