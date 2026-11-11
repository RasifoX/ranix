/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * Casper Nirvana battery and adapter, from the EC's shared RAM, the way the
 * firmware's own AML reads them (DSDT OEM "CASPER", table "ERA"; EC0 methods
 * BATP, ACAP, BCHG, BIF0-BIF4, BSTP/FBST). Pure: no hardware, so
 * tests/host_power.c checks it against byte images.
 *
 * ECW1 is SystemMemory 0xFEFF0380, 0x80 bytes, read byte by byte under the
 * global lock (casper_ec.c does the reading). The fields used:
 *   0x01 EPWS  bit0 adapter in, bit1 battery 0 in its bay
 *   0x02 EB0S  0xFF: no data; bit1 charging
 *   0x06 EB0T  0xFF: battery not identified (AML then returns "unknown")
 *   0x12 B0VL  voltage, mV                 0x14 B0RC  remaining capacity
 *   0x16 B0FC  last full capacity          0x18 B0ST  state, 0x300 = critical
 *   0x1A B0CC  rate, mA (two's complement) 0x50 B0DC  design capacity
 *   0x52 B0DV  design voltage, mV          0x56 B0MD  mode: bit15 0 = capacities in
 *                                                    10 mWh, 1 = in mAh; bit9 0 = rechargeable
 * 16-bit fields are little endian. 0xFFFF means "not known" (the AML's Ones).
 */
#include "casper_bat.h"

static unsigned w16(const uint8_t *w, int off) { return (unsigned)w[off] | ((unsigned)w[off + 1] << 8); }

int casper_ac_decode(const uint8_t *w, ps_t *ac)
{
	ps_t z = { .name = "AC", .type = PS_MAINS };
	*ac = z;
	if (w[0x01] == 0xFF) return -1;               /* the EC answered nothing sensible */
	ac->online = w[0x01] & 1;
	return 0;
}

static void unset(ps_t *b)
{
	b->capacity = -1;
	b->now = b->full = b->design = b->voltage_now = b->current_now = PS_UNSET;
}

int casper_bat_decode(const uint8_t *w, ps_t *b)
{
	ps_t z = { .name = "BAT0", .type = PS_BATTERY };
	*b = z;
	unset(b);
	if (w[0x01] == 0xFF) return -1;
	b->present = (w[0x01] >> 1) & 1;
	if (!b->present) { b->status = PS_ST_UNKNOWN; return 0; }
	if (w[0x06] == 0xFF) { b->status = PS_ST_UNKNOWN; return 0; }   /* GBTT == 0xFF */
	unsigned md = w16(w, 0x56), rc = w16(w, 0x14), fc = w16(w, 0x16), dc = w16(w, 0x50);
	unsigned vl = w16(w, 0x12), st = w16(w, 0x18), cc = w16(w, 0x1A);
	int ac = w[0x01] & 1;
	int chg = w[0x02] != 0xFF && (w[0x02] & 0x02);
	if (md != 0xFFFF) {
		long k = (md >> 15) & 1 ? 1000 : 10000;   /* mAh -> uAh, 10 mWh -> uWh */
		b->energy = !((md >> 15) & 1);
		if (rc != 0xFFFF) b->now = (long)rc * k;
		if (fc != 0xFFFF) b->full = (long)fc * k;
		if (dc != 0xFFFF) b->design = (long)dc * k;
		if (!((md >> 9) & 1)) memcpy(b->technology, "Li-ion", 7);   /* the AML says "LIon" */
	}
	if (rc != 0xFFFF && fc != 0xFFFF && fc) {
		unsigned r = rc > fc ? fc : rc;
		b->capacity = (int)((r * 100u + fc / 2) / fc);
	}
	if (vl != 0xFFFF) b->voltage_now = (long)vl * 1000;
	if (cc != 0xFFFF) {
		/* FBST does 0xFFFF - rate for a negative one (one off from two's complement):
		 * kept as the firmware reports it to Windows and Linux */
		unsigned a = cc >= 0x8000 ? 0xFFFFu - cc : cc;
		b->current_now = (long)a * 1000;
	}
	b->capacity_level_critical = st != 0xFFFF && (st & 0x300);   /* FBST: either bit sets _BST's critical */
	if (!ac) b->status = PS_ST_DISCHARGING;
	else if (chg) b->status = PS_ST_CHARGING;
	else if (b->capacity >= 95) b->status = PS_ST_FULL;
	else b->status = PS_ST_NOT_CHARGING;
	return 0;
}
