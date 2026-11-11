/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * The EC event reader. On the Casper Nirvana, Fn+F11/F12 (volume), the
 * brightness keys, the adapter, the battery and the lid reach the OS only as EC
 * events; its firmware's AML handles them, and Ranix runs no AML. This file asks
 * the EC for its events (through casper_ec.c, the one way into the EC) and does
 * what the AML would:
 *   - the sound keys go into the keyboard path as E0 2E / E0 30 / E0 20, so the
 *     volume changes and programs see KEY_VOLUMEDOWN and friends;
 *   - the brightness keys move the backlight by a tenth;
 *   - adapter, battery and lid events go to casper_power.c.
 * With the SCI on (ranix.sci=1, arch/x86_64/acpi.c) the EC's GPE wakes this
 * thread and it asks only then, plus once every SAFETY_S seconds. Without it,
 * it asks every POLL_MS.
 *
 * Doors: none. Off by default: asking is new traffic to an EC that is still a
 * suspect in this machine's power cuts, so it runs only with ranix.ec_events=1
 * (ranix.ec_keys=1 is the old name) on the command line, and only on a board
 * with a table in ec_hotkey.c (now the Casper, matched by casper_ec.c).
 * /proc/hwdiag "input ec_keys" counts what was seen.
 */
#include <uapi/types.h>
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
#include <drivers/platform/casper_power.h>
#include "ec_hotkey.h"

#define POLL_MS   100
#define SAFETY_S  10
#define KEY_BRIGHTNESSDOWN 224
#define KEY_BRIGHTNESSUP   225
#define BRIGHT_STEP 10

static struct {
	uint32_t polls, events, keys, unknown, busy, timeouts, power, kicks;
	int last;
	volatile int kicked;             /* the SCI saw the EC's GPE */
	int sci;                         /* 1: woken by the SCI, else polling */
} K;

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

/* The SCI handler (acpi.c): the EC raised its GPE. Interrupt context: a flag. */
void ec_events_kick(void)
{
	K.kicked = 1;
	K.kicks++;
}

void ec_events_use_sci(void) { K.sci = 1; }

static void ec_keys_thread(void)
{
	uint64_t last = kernel_ticks;
	casper_power_lid_start();
	for (;;) {
		task_sleep_until(kernel_ticks + POLL_MS * TIMER_HZ / 1000);
		if (K.sci && !K.kicked && kernel_ticks - last < (uint64_t)SAFETY_S * TIMER_HZ) continue;
		K.kicked = 0;
		last = kernel_ticks;
		K.polls++;
		int i;
		for (i = 0; i < 8; i++) {               /* a burst of presses drains in one go */
			int q = casper_ec_event();
			if (q == EC_BUSY) { K.busy++; break; }
			if (q == EC_TIMEOUT) { K.timeouts++; break; }
			if (q == EC_NONE) break;
			K.events++;
			K.last = q;
			int pw = ec_power_casper(q);
			if (pw != ECP_NONE) { K.power++; casper_power_event(pw); }
			else act(ec_hotkey_casper(q));
		}
		if (K.sci) {
			acpi_ec_gpe_rearm();
			/* stopped at 8 with more waiting, or one came while the GPE was off */
			if (i == 8 || casper_ec_pending()) K.kicked = 1;
		}
	}
}

/* Boards without the x86 SCI code: nothing to turn back on. */
__attribute__((weak)) void acpi_ec_gpe_rearm(void) {}

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
	hw_kv(o, "power", K.power);
	hw_kv(o, "sci", (uint64_t)K.sci);
	hw_kv(o, "kicks", K.kicks);
	hw_end(o);
}

void ec_keys_init(void)
{
	char v[8];
	if (!casper_ec_board()) return;
	int on = (cmdline_get("ranix.ec_events", v, sizeof(v)) && !strcmp(v, "1")) ||
	         (cmdline_get("ranix.ec_keys", v, sizeof(v)) && !strcmp(v, "1"));
	if (!on) return;                         /* off unless asked */
	hwdiag_register(ec_keys_diag);
	proc_create("ec_keys", ec_keys_thread);
	kprintf(KERN_INFO "ec_keys: Casper EC events (keys, adapter, battery, lid), %s\n",
	        K.sci ? "woken by the SCI" : "asked every 100 ms");
}
