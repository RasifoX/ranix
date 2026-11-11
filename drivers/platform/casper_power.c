/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * Casper Nirvana: the adapter, the battery and the lid.
 *
 * The battery and the adapter are read from the EC's shared RAM (ECW1, decode in
 * casper_bat.c) every POLL_S seconds and right after an EC power event, and put
 * in /sys/class/power_supply/{AC,BAT0} (kernel/power_supply.c). Reading shared
 * RAM asks the EC to do nothing, so it is on by default on this board;
 * ranix.ec_battery=0 turns it off. A failed read leaves the last values and is
 * counted; three in a row and the supplies say "Unknown" rather than stale data.
 *
 * BAT0 exists only while the EC says a battery is in its bay (EPWS bit1), as on
 * Linux: a machine run on its adapter alone lists AC and nothing else, and a
 * battery taken out leaves the list. "Unknown" after failed reads is said only
 * of a battery that was there; a battery never seen is never invented.
 *
 * The lid is known only through the EC's event _Q81 and its pin RPIN (4, 6), so
 * only when EC events are on (ranix.ec_events=1, ec_keys.c). Then a close or an
 * open is an EV_SW/SW_LID input event, /sys/kernel/lid says "open" or "closed",
 * and an opened lid lights the screen again. Without events it says "unknown".
 *
 * Doors: none (read-only files). /proc/hwdiag "power casper" counts reads.
 */
#include <uapi/types.h>
#include <kernel/cmdline.h>
#include <kernel/fb.h>
#include <kernel/hwdiag.h>
#include <kernel/input.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>
#include <kernel/proc.h>
#include <kernel/time.h>
#include <kernel/board.h>
#include <kernel/power_supply.h>
#include "casper_ec.h"
#include "casper_bat.h"
#include "casper_power.h"
#include <kernel/sync.h>

#define POLL_S     30
#define STALE_READS 3

static struct {
	int on;
	int ac_online;                   /* -1 not read yet */
	int lid;                         /* -1 unknown, 0 closed, 1 open */
	int fails_in_row;
	int bat_listed;                  /* BAT0 is in /sys/class/power_supply */
	uint32_t reads, fails, ac_events, bat_events, lid_events;
} P = { .ac_online = -1, .lid = -1 };

static void publish_unknown(void)
{
	if (!P.bat_listed) return;               /* no battery known: nothing to call unknown */
	ps_t b = { .name = "BAT0", .type = PS_BATTERY, .present = 1, .status = PS_ST_UNKNOWN, .capacity = -1,
	           .now = PS_UNSET, .full = PS_UNSET, .design = PS_UNSET, .voltage_now = PS_UNSET, .current_now = PS_UNSET };
	power_supply_set(&b);
}

static mutex_t refresh_lock = MUTEX_INIT;   /* the poll and an event both refresh: one at a time, in order */

/* One read of ECW1 into the two supplies. Thread context (the EC lock sleeps). */
static void refresh_locked(void);
static void refresh(void)
{
	if (mutex_lock_killable(&refresh_lock)) return;
	refresh_locked();
	mutex_unlock(&refresh_lock);
}

static void refresh_locked(void)
{
	uint8_t w[CASPER_ECW1_LEN];
	ps_t ac, bat;
	if (casper_ec_ram1(w, sizeof(w)) || casper_ac_decode(w, &ac) || casper_bat_decode(w, &bat)) {
		P.fails++;
		if (++P.fails_in_row == STALE_READS) {
			kprintf(KERN_WARN "casper_power: EC RAM unreadable %d times%s\n", STALE_READS,
			        P.bat_listed ? ", battery unknown" : "");
			publish_unknown();
		}
		return;
	}
	P.reads++;
	P.fails_in_row = 0;
	if (P.ac_online != ac.online) {
		const char *how = ac.online ? "in" : "out";
		if (!bat.present) kprintf(KERN_INFO "casper_power: adapter %s\n", how);   /* no battery: not a word */
		else if (bat.capacity < 0) kprintf(KERN_INFO "casper_power: adapter %s, battery charge not known\n", how);
		else kprintf(KERN_INFO "casper_power: adapter %s, battery %d%%\n", how, bat.capacity);
	}
	P.ac_online = ac.online;
	power_supply_set(&ac);
	if (bat.present) {
		if (!P.bat_listed && P.reads > 1) kprintf(KERN_INFO "casper_power: battery in\n");
		power_supply_set(&bat);
		P.bat_listed = 1;
	} else if (P.bat_listed) {               /* taken out: it leaves the list, as on Linux */
		kprintf(KERN_INFO "casper_power: battery out\n");
		power_supply_remove(bat.name);
		P.bat_listed = 0;
	}
}

static void lid_read(void)
{
	int v = casper_ec_pin(4, 6);
	if (v < 0) return;                       /* stays as it was */
	if (v != P.lid) {
		kprintf(KERN_INFO "casper_power: lid %s\n", v ? "open" : "closed");
		input_report_switch(SW_LID, !v);         /* SW_LID 1 = shut, as on Linux */
		if (v) backlight_unblank();
	}
	P.lid = v;
	power_lid_set(v);
}

/* An EC power event (ec_keys.c, the one EC event reader): ECP_* */
void casper_power_event(int kind)
{
	if (!P.on) return;
	if (kind == 1) { P.ac_events++; refresh(); }
	else if (kind == 2) { P.bat_events++; refresh(); }
	else if (kind == 3) { P.lid_events++; lid_read(); }
}

/* Events are on: the lid can be known. Asked once when the event reader starts. */
void casper_power_lid_start(void)
{
	if (P.on) lid_read();
}

int casper_power_lid(void) { return P.lid; }

static void casper_power_thread(void)
{
	for (;;) {
		refresh();
		task_sleep_until(kernel_ticks + (uint64_t)POLL_S * TIMER_HZ);
	}
}

static void casper_power_diag(hw_out_t *o)
{
	hw_begin(o, "power", "casper");
	hw_kv(o, "reads", P.reads);
	hw_kv(o, "fails", P.fails);
	hw_kv(o, "ac_events", P.ac_events);
	hw_kv(o, "bat_events", P.bat_events);
	hw_kv(o, "lid_events", P.lid_events);
	hw_kv(o, "lid", (uint64_t)(P.lid + 1));      /* 0 unknown, 1 closed, 2 open */
	hw_end(o);
}

void casper_power_init(void)
{
	char v[4];
	if (!casper_ec_board()) return;
	if (cmdline_get("ranix.ec_battery", v, sizeof(v)) && !strcmp(v, "0")) {
		kprintf(KERN_INFO "casper_power: off (ranix.ec_battery=0)\n");
		return;
	}
	P.on = 1;
	hwdiag_register(casper_power_diag);
	proc_create("casper_power", casper_power_thread);
	kprintf(KERN_INFO "casper_power: battery and adapter from EC RAM every %d s\n", POLL_S);
}
