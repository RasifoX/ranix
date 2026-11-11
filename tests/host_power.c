/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Host test: the Casper battery decode (drivers/platform/casper_bat.c) on EC RAM
 * images, and the /sys/class/power_supply text (kernel/lib/ps_format.c).
 *   cc -DHOST_TEST -I. -Iinclude tests/host_power.c drivers/platform/casper_bat.c kernel/lib/ps_format.c */
#include <stdio.h>
#include <string.h>
#include "drivers/platform/casper_bat.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static void put16(uint8_t *w, int off, unsigned v) { w[off] = (uint8_t)v; w[off + 1] = (uint8_t)(v >> 8); }

/* A battery as the EC keeps it: mWh mode (B0MD bit15 0), 4100 x 10 mWh design */
static void image(uint8_t *w, int ac, int charging, unsigned rc, unsigned fc)
{
	memset(w, 0, CASPER_ECW1_LEN);
	w[0x01] = (uint8_t)((ac ? 1 : 0) | 2);           /* EPWS: adapter, battery 0 in */
	w[0x02] = (uint8_t)(charging ? 0x03 : 0x01);     /* EB0S */
	w[0x06] = 0x01;                                  /* EB0T: identified */
	put16(w, 0x12, 11400);                           /* B0VL mV */
	put16(w, 0x14, rc);                              /* B0RC */
	put16(w, 0x16, fc);                              /* B0FC */
	put16(w, 0x18, 0);                               /* B0ST */
	put16(w, 0x1A, charging ? 1500 : 0xFFFF - 900);  /* B0CC: 1.5 A in, 0.9 A out */
	put16(w, 0x50, 4100);                            /* B0DC */
	put16(w, 0x52, 11400);                           /* B0DV */
	put16(w, 0x56, 0x0000);                          /* B0MD: mWh, rechargeable */
}

static const char *attr(const ps_t *p, const char *a)
{
	static char b[512];
	return ps_attr_format(p, a, b, sizeof b) < 0 ? NULL : b;
}

static int has_file(const ps_t *p, const char *a)
{
	for (int i = 0; ps_attr_name(p, i); i++)
		if (!strcmp(ps_attr_name(p, i), a)) return 1;
	return 0;
}

int main(void)
{
	uint8_t w[CASPER_ECW1_LEN];
	ps_t ac, b;

	image(w, 0, 0, 2050, 4000);
	CHECK(casper_ac_decode(w, &ac) == 0 && !ac.online, "adapter out");
	CHECK(casper_bat_decode(w, &b) == 0 && b.present, "battery in");
	CHECK(b.capacity == 51, "capacity %d (2050/4000)", b.capacity);
	CHECK(b.status == PS_ST_DISCHARGING, "discharging");
	CHECK(b.energy && b.now == 20500000 && b.full == 40000000 && b.design == 41000000, "energy uWh %ld %ld %ld", b.now, b.full, b.design);
	CHECK(b.voltage_now == 11400000 && b.current_now == 900000, "volt %ld curr %ld", b.voltage_now, b.current_now);
	CHECK(!strcmp(attr(&b, "status"), "Discharging\n") && !strcmp(attr(&b, "capacity"), "51\n"), "text");
	CHECK(!strcmp(attr(&b, "type"), "Battery\n") && !strcmp(attr(&ac, "type"), "Mains\n") && !strcmp(attr(&ac, "online"), "0\n"), "types");
	CHECK(!strcmp(attr(&b, "technology"), "Li-ion\n") && !strcmp(attr(&b, "capacity_level"), "Normal\n"), "tech/level");
	CHECK(has_file(&b, "energy_now") && !has_file(&b, "charge_now") && !has_file(&b, "online") && !has_file(&ac, "capacity"), "file set");
	const char *u = attr(&b, "uevent");
	CHECK(u && strstr(u, "POWER_SUPPLY_NAME=BAT0\n") && strstr(u, "POWER_SUPPLY_CAPACITY=51\n") &&
	      strstr(u, "POWER_SUPPLY_STATUS=Discharging\n") && !strstr(u, "UEVENT"), "uevent: %s", u ? u : "-");

	image(w, 1, 1, 3000, 4000);
	casper_ac_decode(w, &ac);
	casper_bat_decode(w, &b);
	CHECK(ac.online && b.status == PS_ST_CHARGING && b.capacity == 75 && b.current_now == 1500000, "charging");
	image(w, 1, 0, 4000, 4000);
	casper_bat_decode(w, &b);
	CHECK(b.status == PS_ST_FULL && b.capacity == 100 && !strcmp(attr(&b, "capacity_level"), "Full\n"), "full");
	image(w, 1, 0, 2000, 4000);
	casper_bat_decode(w, &b);
	CHECK(b.status == PS_ST_NOT_CHARGING, "plugged, not charging");
	image(w, 0, 0, 4100, 4000);                      /* remaining above full: never over 100 */
	casper_bat_decode(w, &b);
	CHECK(b.capacity == 100, "clamped %d", b.capacity);
	image(w, 0, 0, 120, 4000);
	put16(w, 0x18, 0x0200);                          /* B0ST critical */
	casper_bat_decode(w, &b);
	CHECK(b.capacity == 3 && !strcmp(attr(&b, "capacity_level"), "Critical\n"), "critical");

	/* mAh mode (B0MD bit15): charge files, uAh */
	image(w, 0, 0, 1000, 2000);
	put16(w, 0x56, 0x8000);
	casper_bat_decode(w, &b);
	CHECK(!b.energy && b.now == 1000000 && has_file(&b, "charge_now") && !has_file(&b, "energy_now"), "mAh");

	/* what is not known has no file, and nothing is made up */
	image(w, 0, 0, 0xFFFF, 0xFFFF);
	put16(w, 0x12, 0xFFFF); put16(w, 0x1A, 0xFFFF);
	casper_bat_decode(w, &b);
	CHECK(b.present && b.capacity == -1 && !has_file(&b, "capacity") && !has_file(&b, "voltage_now") &&
	      !has_file(&b, "current_now") && !has_file(&b, "energy_now"), "unknown values: no files");
	image(w, 0, 0, 2000, 4000);
	w[0x06] = 0xFF;                                  /* EB0T: not identified */
	casper_bat_decode(w, &b);
	CHECK(b.present && b.status == PS_ST_UNKNOWN && !has_file(&b, "capacity") && !strcmp(attr(&b, "status"), "Unknown\n"), "unidentified");
	image(w, 1, 0, 2000, 4000);
	w[0x01] = 1;                                     /* battery out of its bay */
	casper_bat_decode(w, &b);
	CHECK(!b.present && !strcmp(attr(&b, "present"), "0\n") && !has_file(&b, "capacity"), "no battery");
	/* the bay empty, the capacity fields 0 or unknown (0xFFFF, the AML's Ones):
	 * absent, no percentage, no capacity files (casper_power.c then lists no BAT0) */
	unsigned cap_vals[2] = { 0, 0xFFFF };
	for (int v = 0; v < 2; v++) {
		image(w, 1, 0, cap_vals[v], cap_vals[v]);
		w[0x01] = 1;
		put16(w, 0x50, cap_vals[v]);
		casper_bat_decode(w, &b);
		CHECK(!b.present && b.capacity == -1 && !has_file(&b, "capacity") && !has_file(&b, "capacity_level") &&
		      !has_file(&b, "energy_full") && !has_file(&b, "energy_full_design"), "no battery, capacity fields 0x%x", cap_vals[v]);
	}
	/* in its bay but full capacity 0 or unknown: no division, no percentage */
	for (int v = 0; v < 2; v++) {
		image(w, 1, 0, 2000, cap_vals[v]);
		put16(w, 0x50, cap_vals[v]);
		casper_bat_decode(w, &b);
		CHECK(b.present && b.capacity == -1 && !has_file(&b, "capacity") && !has_file(&b, "capacity_level"),
		      "full 0x%x: no percentage (%d)", cap_vals[v], b.capacity);
	}
	image(w, 1, 0, 0xFFFF, 4000);                    /* remaining unknown */
	casper_bat_decode(w, &b);
	CHECK(b.present && b.capacity == -1 && !has_file(&b, "capacity"), "remaining unknown: no percentage");
	memset(w, 0xFF, sizeof w);                       /* an EC that answers nothing */
	CHECK(casper_ac_decode(w, &ac) == -1 && casper_bat_decode(w, &b) == -1, "garbage refused");
	char tiny[4];
	CHECK(ps_attr_format(&ac, "type", tiny, sizeof tiny) == 3 && !strcmp(tiny, "Mai"), "bounded");
	CHECK(ps_attr_format(&ac, "capacity", tiny, sizeof tiny) == -1, "no such file");

	printf("host_power: %d/%d\n", checks - fails, checks);
	return fails ? 1 : 0;
}
