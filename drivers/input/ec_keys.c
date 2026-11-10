/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * Fn keys that are embedded-controller events, not scancodes. On the Casper
 * Nirvana, Fn+F11/F12 (volume) and the brightness keys reach the OS only as EC
 * events; its firmware's AML turns them into keys, and Ranix runs no AML. This
 * file asks the EC for its events every 50 ms (Ranix takes no SCI) and does what
 * the AML would: the sound keys go into the keyboard path as E0 2E / E0 30 /
 * E0 20, so the volume changes and programs see KEY_VOLUMEDOWN and friends;
 * the brightness keys move the backlight by a tenth.
 *
 * Doors: none. Only a board with a table in ec_hotkey.c is polled (now the
 * Casper, matched by casper_ec.c); everywhere else the EC ports are not touched.
 * ranix.ec_keys=0 on the command line turns it off. /proc/hwdiag "input
 * ec_keys" counts what was seen.
 */
#include <uapi/types.h>
#include <asm/io.h>
#include <kernel/cmdline.h>
#include <kernel/fb.h>
#include <kernel/hwdiag.h>
#include <kernel/init.h>
#include <kernel/input.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>
#include <kernel/proc.h>
#include <kernel/time.h>
#include <kernel/board.h>
#include <drivers/input/ps2.h>
#include <drivers/platform/casper_ec.h>
#include "ec_hotkey.h"

#define POLL_MS 50
#define KEY_BRIGHTNESSDOWN 224
#define KEY_BRIGHTNESSUP   225
#define BRIGHT_STEP 10

static struct {
	uint32_t polls, events, keys, unknown, busy, timeouts;
	int last;
} K;

static uint8_t port_in(uint16_t p) { return inb(p); }
static void port_out(uint16_t p, uint8_t v) { outb(p, v); }
static void port_wait(void) { for (int i = 0; i < 10; i++) (void)inb(0x80); }   /* ~1 us each */
static const ec_io_t io = {port_in, port_out, port_wait};

static void press_e0(uint16_t sc)
{
	input_usb_key((uint16_t)(0x100 | sc), 1);
	input_usb_key((uint16_t)(0x100 | sc), 0);
}

static void brightness(int dir, uint16_t key)
{
	int v = backlight_get();
	if (v >= 0) {
		v += dir * BRIGHT_STEP;
		backlight_set(v < 5 ? 5 : v > 100 ? 100 : v);
	}
	backlight_unblank();
	input_report_key(key, 1);
	input_sync();
	input_report_key(key, 0);
	input_sync();
}

static void act(int a)
{
	switch (a) {
	case ECA_MUTE:        press_e0(0x20); break;
	case ECA_VOL_DOWN:    press_e0(0x2E); break;
	case ECA_VOL_UP:      press_e0(0x30); break;
	case ECA_BRIGHT_DOWN: brightness(-1, KEY_BRIGHTNESSDOWN); break;
	case ECA_BRIGHT_UP:   brightness(1, KEY_BRIGHTNESSUP); break;
	default:              K.unknown++; return;
	}
	K.keys++;
}

static void ec_keys_thread(void)
{
	for (;;) {
		task_sleep_until(kernel_ticks + POLL_MS * TIMER_HZ / 1000);
		K.polls++;
		for (int i = 0; i < 8; i++) {           /* a burst of presses drains in one go */
			int q = ec_query(&io);
			if (q == EC_BUSY) { K.busy++; break; }
			if (q == EC_TIMEOUT) { K.timeouts++; break; }
			if (q == EC_NONE) break;
			K.events++;
			K.last = q;
			act(ec_hotkey_casper(q));
		}
	}
}

static void ec_keys_diag(hw_out_t *o)
{
	hw_begin(o, "input", "ec_keys");
	hw_kv(o, "polls", K.polls);
	hw_kv(o, "events", K.events);
	hw_kv(o, "last", (uint64_t)K.last);
	hw_kv(o, "keys", K.keys);
	hw_kv(o, "unknown", K.unknown);
	hw_kv(o, "busy", K.busy);
	hw_kv(o, "timeouts", K.timeouts);
	hw_end(o);
}

void ec_keys_init(void)
{
	char v[8];
	if (!casper_ec_board()) return;
	if (cmdline_get("ranix.ec_keys", v, sizeof(v)) && !strcmp(v, "0")) return;
	hwdiag_register(ec_keys_diag);
	proc_create("ec_keys", ec_keys_thread);
	kprintf(KERN_INFO "ec_keys: Casper Fn keys from EC events, every %d ms\n", POLL_MS);
}
