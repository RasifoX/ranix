/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef DRIVERS_INPUT_HIDJOY_H
#define DRIVERS_INPUT_HIDJOY_H
#if __has_include(<uapi/types.h>)
#include <uapi/types.h>
#else
#include <stdint.h>
#endif

/*
 * Joysticks and gamepads, the part that is only arithmetic. A HID report
 * descriptor becomes a list of fields (where each axis, hat and button sits in a
 * report), and a report becomes evdev events (EV_KEY, EV_ABS) with Linux's codes,
 * so SDL, GLFW and Wine see the device as they would on Linux. Xbox 360 style
 * pads (XInput: vendor class 0xFF/0x5D/0x01, Logitech F310 in X mode) are not HID
 * and have a fixed 20-byte report, decoded by hj_xpad_report.
 * No hardware here: tests/host_hidjoy.c checks it on the host.
 *
 * Sources, all public: USB HID 1.11 and the HID Usage Tables (report descriptor
 * items, Generic Desktop and Button pages); Linux's input-event-codes.h numbers
 * (an ABI); the free60 project's notes on the Xbox 360 controller report.
 */

#define HJ_EV_KEY 0x01
#define HJ_EV_ABS 0x03
#define HJ_ABS_X 0x00
#define HJ_ABS_Y 0x01
#define HJ_ABS_Z 0x02
#define HJ_ABS_RX 0x03
#define HJ_ABS_RY 0x04
#define HJ_ABS_RZ 0x05
#define HJ_ABS_THROTTLE 0x06
#define HJ_ABS_RUDDER 0x07
#define HJ_ABS_WHEEL 0x08
#define HJ_ABS_GAS 0x09
#define HJ_ABS_BRAKE 0x0a
#define HJ_ABS_HAT0X 0x10
#define HJ_ABS_HAT0Y 0x11
#define HJ_BTN_JOYSTICK 0x120   /* BTN_TRIGGER: joystick buttons 1..16 */
#define HJ_BTN_GAMEPAD 0x130    /* BTN_SOUTH (A): gamepad buttons 1..16 */
#define HJ_BTN_HAPPY 0x2c0      /* BTN_TRIGGER_HAPPY1: buttons 17.. */
#define HJ_KEY_MAX 0x2ff

#define HJ_AXIS 1
#define HJ_HAT  2
#define HJ_BTNS 3

/* One input field: count values of size bits each, from bit off of the report
 * (after its id byte, if it has one). */
typedef struct {
	uint8_t  kind, id, count, sign;
	uint16_t off, size;
	uint16_t code;             /* ABS code, or the first button's KEY code */
	int32_t  lmin, lmax;
} hj_field_t;

#define HJ_MAXF 24
typedef struct {
	uint8_t    nf, gamepad, ids;   /* ids: the reports carry an id byte */
	hj_field_t f[HJ_MAXF];
} hidjoy_t;

/* What the device can report, for evdev's EVIOCGBIT and EVIOCGABS. */
typedef struct {
	uint64_t keybits[(HJ_KEY_MAX + 64) / 64];
	uint64_t absbits;
	int32_t  absmin[64], absmax[64];
} hj_caps_t;

typedef void (*hj_emit_t)(void *ctx, uint16_t type, uint16_t code, int32_t value);

/* A report descriptor: 1 when it is a joystick, gamepad or multi-axis controller. */
int hj_parse(const uint8_t *p, uint32_t n, hidjoy_t *j);
void hj_caps(const hidjoy_t *j, hj_caps_t *c);
/* One report (with its id byte when the device uses ids): every value it holds. */
void hj_report(const hidjoy_t *j, const uint8_t *r, uint32_t len, hj_emit_t emit, void *ctx);

/* Xbox 360 style (XInput) pads */
void hj_xpad_caps(hj_caps_t *c);
void hj_xpad_report(const uint8_t *r, uint32_t len, hj_emit_t emit, void *ctx);

#endif
