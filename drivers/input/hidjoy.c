/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * Joysticks and gamepads: report descriptors to fields, reports to evdev events.
 * Pure functions; see hidjoy.h for what each does and where it comes from.
 */
#include "hidjoy.h"

/* Generic Desktop usages 0x30.. (X, Y, Z, Rx, Ry, Rz, Slider, Dial, Wheel) */
static const uint8_t gd_abs[9] = {
	HJ_ABS_X, HJ_ABS_Y, HJ_ABS_Z, HJ_ABS_RX, HJ_ABS_RY, HJ_ABS_RZ,
	HJ_ABS_THROTTLE, HJ_ABS_RUDDER, HJ_ABS_WHEEL
};

/* An axis usage (page << 16 | id) to its ABS code; -1 if not an axis. */
static int abs_code(uint32_t u)
{
	uint32_t page = u >> 16, id = u & 0xFFFF;
	if (page == 1 && id >= 0x30 && id <= 0x38) return gd_abs[id - 0x30];
	if (page == 2 && id == 0xBA) return HJ_ABS_RUDDER;
	if (page == 2 && id == 0xBB) return HJ_ABS_THROTTLE;
	if (page == 2 && id == 0xC4) return HJ_ABS_GAS;
	if (page == 2 && id == 0xC5) return HJ_ABS_BRAKE;
	return -1;
}

static int have_abs(const hidjoy_t *j, int code)
{
	for (int i = 0; i < j->nf; i++)
		if (j->f[i].kind == HJ_AXIS && j->f[i].code == code) return 1;
	return 0;
}

static int32_t sext(uint32_t v, uint32_t sz)
{
	if (sz && sz < 4 && (v >> (sz * 8 - 1) & 1)) v |= ~0u << (sz * 8);
	return (int32_t)v;
}

typedef struct {
	uint32_t page, rsize, rcount, id;
	int32_t  lmin, lmax;
	uint32_t umax_unsigned;           /* lmax as written, for a descriptor that meant it unsigned */
	uint32_t usage[16], nu, umin, umax;
	int      depth, app_at, app;
	uint16_t off[256];
} hj_state_t;

/* An input item inside the application collection: its fields. */
static void hj_input(hidjoy_t *j, hj_state_t *s, uint32_t v)
{
	uint16_t *o = &s->off[s->id & 0xFF];
	uint32_t base = *o, total = s->rsize * s->rcount;
	*o = (uint16_t)(*o + total);
	if (s->app_at < 0 || (v & 1) || !(v & 2) || (v & 4) || !s->rsize) return;
	int32_t lmin = s->lmin, lmax = s->lmax;
	if (lmax < lmin) lmax = (int32_t)s->umax_unsigned;
	for (uint32_t k = 0; k < s->rcount && j->nf < HJ_MAXF; k++) {
		uint32_t u = k < s->nu ? s->usage[k] : s->umin ? s->umin + k : s->nu ? s->usage[s->nu - 1] : 0;
		if (s->umin && k >= s->nu && u > s->umax) break;
		hj_field_t *f = &j->f[j->nf];
		f->id = (uint8_t)s->id;
		f->off = (uint16_t)(base + k * s->rsize);
		f->size = (uint16_t)s->rsize;
		f->lmin = lmin;
		f->lmax = lmax;
		f->sign = lmin < 0;
		f->count = 1;
		if ((u >> 16) == 9 && (u & 0xFFFF)) {              /* buttons: one field for the run */
			f->kind = HJ_BTNS;
			f->code = (uint16_t)(u & 0xFFFF);
			f->count = (uint8_t)(s->rcount - k > 64 ? 64 : s->rcount - k);
			j->nf++;
			return;
		}
		if (u == 0x10039) {                                /* hat switch */
			f->kind = HJ_HAT;
			j->nf++;
			continue;
		}
		int c = abs_code(u);
		if (c < 0 || have_abs(j, c)) continue;
		f->kind = HJ_AXIS;
		f->code = (uint16_t)c;
		j->nf++;
	}
}

/* One main or global item; 0 to stop (the application collection ended). */
static int hj_item(hidjoy_t *j, hj_state_t *s, uint8_t type, uint8_t tag, uint32_t v, uint32_t sz)
{
	if (type == 1) {                                       /* global */
		if (tag == 0) s->page = v;
		else if (tag == 1) s->lmin = sext(v, sz);
		else if (tag == 2) { s->lmax = sext(v, sz); s->umax_unsigned = v; }
		else if (tag == 7) s->rsize = v;
		else if (tag == 8) { s->id = v; if (v) j->ids = 1; }
		else if (tag == 9) s->rcount = v;
		return 1;
	}
	if (type == 2) {                                       /* local */
		uint32_t u = sz == 4 ? v : (s->page << 16 | v);
		if (tag == 0 && s->nu < 16) s->usage[s->nu++] = u;
		else if (tag == 1) s->umin = u;
		else if (tag == 2) s->umax = u;
		return 1;
	}
	if (type != 0) return 1;
	if (tag == 10) {                                       /* collection */
		uint32_t u = s->nu ? s->usage[0] : 0;
		if (v == 1 && s->app_at < 0 && (u == 0x10004 || u == 0x10005 || u == 0x10008)) {
			s->app_at = s->depth;
			j->gamepad = u == 0x10005;
		}
		s->depth++;
	} else if (tag == 12) {                                /* end collection */
		s->depth--;
		if (s->depth == s->app_at) return 0;
	} else if (tag == 8) {                                 /* input (output and feature: not ours) */
		hj_input(j, s, v);
	}
	s->nu = 0;
	s->umin = s->umax = 0;
	return 1;
}

int hj_parse(const uint8_t *p, uint32_t n, hidjoy_t *j)
{
	static hj_state_t s;                                   /* 2 KB: not on a kernel stack */
	for (uint32_t i = 0; i < sizeof s; i++) ((uint8_t *)&s)[i] = 0;
	for (uint32_t i = 0; i < sizeof *j; i++) ((uint8_t *)j)[i] = 0;
	s.app_at = -1;
	for (uint32_t i = 0; i < n; ) {
		uint8_t it = p[i++];
		if (it == 0xFE) {                                  /* long item */
			if (i + 1 >= n) break;
			i += 2u + p[i];
			continue;
		}
		uint32_t sz = (it & 3) == 3 ? 4 : (it & 3), v = 0;
		if (i + sz > n) break;
		for (uint32_t k = 0; k < sz; k++) v |= (uint32_t)p[i + k] << (k * 8);
		i += sz;
		if (!hj_item(j, &s, (it >> 2) & 3, it >> 4, v, sz)) break;
	}
	int axes = 0;
	for (int i = 0; i < j->nf; i++) axes += j->f[i].kind != HJ_BTNS;
	return s.app_at >= 0 && axes > 0;
}

static uint16_t btn_code(const hidjoy_t *j, uint32_t u)
{
	if (u >= 1 && u <= 16) return (uint16_t)((j->gamepad ? HJ_BTN_GAMEPAD : HJ_BTN_JOYSTICK) + u - 1);
	return (uint16_t)(HJ_BTN_HAPPY + (u - 17));
}

static void set_key(hj_caps_t *c, uint32_t code)
{
	if (code <= HJ_KEY_MAX) c->keybits[code / 64] |= 1ULL << (code % 64);
}

static void set_abs(hj_caps_t *c, int code, int32_t min, int32_t max)
{
	c->absbits |= 1ULL << code;
	c->absmin[code] = min;
	c->absmax[code] = max;
}

void hj_caps(const hidjoy_t *j, hj_caps_t *c)
{
	for (uint32_t i = 0; i < sizeof *c; i++) ((uint8_t *)c)[i] = 0;
	for (int i = 0; i < j->nf; i++) {
		const hj_field_t *f = &j->f[i];
		if (f->kind == HJ_AXIS) set_abs(c, f->code, f->lmin, f->lmax);
		else if (f->kind == HJ_HAT) { set_abs(c, HJ_ABS_HAT0X, -1, 1); set_abs(c, HJ_ABS_HAT0Y, -1, 1); }
		else for (uint32_t k = 0; k < f->count && f->code + k <= 80; k++) set_key(c, btn_code(j, f->code + k));
	}
}

static uint32_t bits(const uint8_t *r, uint32_t len, uint32_t off, uint32_t size)
{
	uint32_t v = 0;
	for (uint32_t i = 0; i < size && i < 32; i++) {
		uint32_t b = off + i;
		if (b / 8 < len && (r[b / 8] >> (b % 8) & 1)) v |= 1u << i;
	}
	return v;
}

void hj_report(const hidjoy_t *j, const uint8_t *r, uint32_t len, hj_emit_t emit, void *ctx)
{
	static const int8_t hx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 }, hy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
	uint32_t id = j->ids && len ? r[0] : 0, skip = j->ids ? 8 : 0;
	for (int i = 0; i < j->nf; i++) {
		const hj_field_t *f = &j->f[i];
		if (f->id != id) continue;
		uint32_t off = f->off + skip;
		if ((off + (uint32_t)f->size * f->count + 7) / 8 > len) continue;   /* a short report */
		if (f->kind == HJ_BTNS) {
			for (uint32_t k = 0; k < f->count && f->code + k <= 80; k++)
				emit(ctx, HJ_EV_KEY, btn_code(j, f->code + k), (int32_t)bits(r, len, off + k * f->size, 1));
			continue;
		}
		uint32_t raw = bits(r, len, off, f->size);
		int32_t v = (int32_t)raw;
		if (f->sign && f->size < 32 && (raw >> (f->size - 1) & 1)) v = (int32_t)(raw | ~0u << f->size);
		if (f->kind == HJ_AXIS) {
			emit(ctx, HJ_EV_ABS, f->code, v);
			continue;
		}
		int32_t span = f->lmax - f->lmin + 1, d = v - f->lmin;   /* hat: 8 or 4 directions */
		if (span == 4) d *= 2;
		int ok = d >= 0 && d < 8 && v <= f->lmax;
		emit(ctx, HJ_EV_ABS, HJ_ABS_HAT0X, ok ? hx[d] : 0);
		emit(ctx, HJ_EV_ABS, HJ_ABS_HAT0Y, ok ? hy[d] : 0);
	}
}

/* ---- Xbox 360 style (XInput) ---- */

/* bytes 2 and 3 of the report, bit by bit (dpad bits are axes, 0 here) */
static const uint16_t xpad_btn[16] = {
	0, 0, 0, 0, 0x13b, 0x13a, 0x13d, 0x13e,          /* start, back, left and right stick */
	0x136, 0x137, 0x13c, 0, 0x130, 0x131, 0x133, 0x134   /* LB, RB, guide, -, A, B, X, Y */
};

void hj_xpad_caps(hj_caps_t *c)
{
	for (uint32_t i = 0; i < sizeof *c; i++) ((uint8_t *)c)[i] = 0;
	for (int i = 0; i < 16; i++) if (xpad_btn[i]) set_key(c, xpad_btn[i]);
	set_abs(c, HJ_ABS_X, -32768, 32767);
	set_abs(c, HJ_ABS_Y, -32768, 32767);
	set_abs(c, HJ_ABS_RX, -32768, 32767);
	set_abs(c, HJ_ABS_RY, -32768, 32767);
	set_abs(c, HJ_ABS_Z, 0, 255);
	set_abs(c, HJ_ABS_RZ, 0, 255);
	set_abs(c, HJ_ABS_HAT0X, -1, 1);
	set_abs(c, HJ_ABS_HAT0Y, -1, 1);
}

static int32_t le_s16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

/* Bytes: 0 type (0), 1 length (20), 2-3 buttons, 4 LT, 5 RT, 6-13 LX LY RX RY.
 * Stick Y is up-positive on the wire; evdev's is down-positive, as xpad does it. */
void hj_xpad_report(const uint8_t *r, uint32_t len, hj_emit_t emit, void *ctx)
{
	if (len < 14 || r[0] != 0x00 || r[1] < 14) return;     /* not an input report */
	uint32_t b = r[2] | (uint32_t)r[3] << 8;
	for (int i = 0; i < 16; i++)
		if (xpad_btn[i]) emit(ctx, HJ_EV_KEY, xpad_btn[i], (int32_t)(b >> i & 1));
	emit(ctx, HJ_EV_ABS, HJ_ABS_HAT0X, (b & 8 ? 1 : 0) - (b & 4 ? 1 : 0));
	emit(ctx, HJ_EV_ABS, HJ_ABS_HAT0Y, (b & 2 ? 1 : 0) - (b & 1 ? 1 : 0));
	emit(ctx, HJ_EV_ABS, HJ_ABS_Z, r[4]);
	emit(ctx, HJ_EV_ABS, HJ_ABS_RZ, r[5]);
	emit(ctx, HJ_EV_ABS, HJ_ABS_X, le_s16(r + 6));
	emit(ctx, HJ_EV_ABS, HJ_ABS_Y, ~le_s16(r + 8));
	emit(ctx, HJ_EV_ABS, HJ_ABS_RX, le_s16(r + 10));
	emit(ctx, HJ_EV_ABS, HJ_ABS_RY, ~le_s16(r + 12));
}

/* ---- Xbox One / Series (GIP) ---- */

/* message 0x20, bytes 4 and 5, bit by bit (dpad bits are axes, 0 here) */
static const uint16_t gip_btn[16] = {
	0, 0, 0x13b, 0x13a, 0x130, 0x131, 0x133, 0x134,   /* -, -, menu, view, A, B, X, Y */
	0, 0, 0, 0, 0x136, 0x137, 0x13d, 0x13e             /* dpad, LB, RB, left and right stick */
};

void hj_gip_caps(hj_caps_t *c)
{
	hj_xpad_caps(c);                  /* the same buttons and sticks */
	set_abs(c, HJ_ABS_Z, 0, 1023);    /* triggers: 10 bits */
	set_abs(c, HJ_ABS_RZ, 0, 1023);
}

uint32_t hj_gip_init(uint8_t *b, int n, uint8_t seq)
{
	static const uint8_t power_on[5] = { 0x05, 0x20, 0x00, 0x01, 0x00 };
	static const uint8_t led_on[7] = { 0x0A, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14 };
	const uint8_t *m = n == 0 ? power_on : n == 1 ? led_on : 0;
	uint32_t len = n == 0 ? sizeof power_on : n == 1 ? sizeof led_on : 0;
	for (uint32_t i = 0; i < len; i++) b[i] = m[i];
	if (len) b[2] = seq;
	return len;
}

void hj_gip_report(const uint8_t *r, uint32_t len, hj_emit_t emit, void *ctx)
{
	if (len >= 5 && r[0] == 0x07) {                      /* the Xbox button */
		emit(ctx, HJ_EV_KEY, 0x13c, r[4] & 1);
		return;
	}
	if (len < 18 || r[0] != 0x20) return;
	uint32_t b = r[4] | (uint32_t)r[5] << 8;
	for (int i = 0; i < 16; i++)
		if (gip_btn[i]) emit(ctx, HJ_EV_KEY, gip_btn[i], (int32_t)(b >> i & 1));
	emit(ctx, HJ_EV_ABS, HJ_ABS_HAT0X, (b & 0x800 ? 1 : 0) - (b & 0x400 ? 1 : 0));
	emit(ctx, HJ_EV_ABS, HJ_ABS_HAT0Y, (b & 0x200 ? 1 : 0) - (b & 0x100 ? 1 : 0));
	emit(ctx, HJ_EV_ABS, HJ_ABS_Z, (r[6] | r[7] << 8) & 0x3FF);
	emit(ctx, HJ_EV_ABS, HJ_ABS_RZ, (r[8] | r[9] << 8) & 0x3FF);
	emit(ctx, HJ_EV_ABS, HJ_ABS_X, le_s16(r + 10));
	emit(ctx, HJ_EV_ABS, HJ_ABS_Y, ~le_s16(r + 12));
	emit(ctx, HJ_EV_ABS, HJ_ABS_RX, le_s16(r + 14));
	emit(ctx, HJ_EV_ABS, HJ_ABS_RY, ~le_s16(r + 16));
}
