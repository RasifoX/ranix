/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The files of one /sys/class/power_supply/<name>/ directory and their text, as
 * Linux writes them (Documentation/ABI/testing/sysfs-class-power). Pure: no
 * kernel calls, so tests/host_power.c checks it on a host. A value the driver
 * does not know has no file; nothing is guessed. */
#ifdef HOST_TEST
#include <string.h>
#else
#include <kernel/lib/string.h>
#endif
#include <kernel/power_supply.h>

enum { A_TYPE, A_UEVENT, A_ONLINE, A_PRESENT, A_STATUS, A_CAPACITY, A_LEVEL, A_TECH,
       A_NOW, A_FULL, A_DESIGN, A_VOLT, A_CURR, A_N };

static const char *const ENERGY[3] = { "energy_now", "energy_full", "energy_full_design" };
static const char *const CHARGE[3] = { "charge_now", "charge_full", "charge_full_design" };

static int has(const ps_t *p, int a)
{
	int bat = p->type == PS_BATTERY;
	switch (a) {
	case A_TYPE: case A_UEVENT: return 1;
	case A_ONLINE:   return !bat;
	case A_PRESENT:  return bat;
	case A_STATUS:   return bat;
	case A_CAPACITY: return bat && p->present && p->capacity >= 0;
	case A_LEVEL:    return bat && p->present && p->capacity >= 0;
	case A_TECH:     return bat && p->present && p->technology[0];
	case A_NOW:      return bat && p->present && p->now != PS_UNSET;
	case A_FULL:     return bat && p->present && p->full != PS_UNSET;
	case A_DESIGN:   return bat && p->present && p->design != PS_UNSET;
	case A_VOLT:     return bat && p->present && p->voltage_now != PS_UNSET;
	case A_CURR:     return bat && p->present && p->current_now != PS_UNSET;
	}
	return 0;
}

static const char *name_of(const ps_t *p, int a)
{
	static const char *const N[A_N] = { "type", "uevent", "online", "present", "status", "capacity",
	                                     "capacity_level", "technology", NULL, NULL, NULL,
	                                     "voltage_now", "current_now" };
	if (a == A_NOW || a == A_FULL || a == A_DESIGN) return (p->energy ? ENERGY : CHARGE)[a - A_NOW];
	return N[a];
}

int ps_attr_count(const ps_t *p)
{
	int n = 0;
	for (int a = 0; a < A_N; a++) n += has(p, a);
	return n;
}

const char *ps_attr_name(const ps_t *p, int i)
{
	for (int a = 0; a < A_N; a++)
		if (has(p, a) && i-- == 0) return name_of(p, a);
	return NULL;
}

/* ---- a tiny bounded writer ---- */
typedef struct { char *b; size_t cap, n; } w_t;
static void ws(w_t *w, const char *s) { while (*s) { if (w->n + 1 < w->cap) w->b[w->n] = *s; w->n++; s++; } }
static void wl(w_t *w, long v)
{
	char t[24];
	int n = 0;
	unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
	do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
	if (v < 0) ws(w, "-");
	while (n) { char c[2] = { t[--n], 0 }; ws(w, c); }
}

static const char *status_word(int s)
{
	switch (s) {
	case PS_ST_CHARGING:     return "Charging";
	case PS_ST_DISCHARGING:  return "Discharging";
	case PS_ST_NOT_CHARGING: return "Not charging";
	case PS_ST_FULL:         return "Full";
	default:                 return "Unknown";
	}
}

/* Linux: Critical below 5, Low below 15... as the battery class guesses it when
 * the hardware gives only a percent; the hardware's critical flag wins. */
static const char *level_word(const ps_t *p)
{
	if (p->capacity_level_critical || p->capacity < 5) return "Critical";
	if (p->capacity < 15) return "Low";
	if (p->status == PS_ST_FULL || p->capacity >= 100) return "Full";
	if (p->capacity >= 90) return "High";
	return "Normal";
}

static void value(w_t *w, const ps_t *p, int a)
{
	switch (a) {
	case A_TYPE:     ws(w, p->type == PS_BATTERY ? "Battery" : "Mains"); break;
	case A_ONLINE:   wl(w, p->online); break;
	case A_PRESENT:  wl(w, p->present); break;
	case A_STATUS:   ws(w, status_word(p->status)); break;
	case A_CAPACITY: wl(w, p->capacity); break;
	case A_LEVEL:    ws(w, level_word(p)); break;
	case A_TECH:     ws(w, p->technology); break;
	case A_NOW:      wl(w, p->now); break;
	case A_FULL:     wl(w, p->full); break;
	case A_DESIGN:   wl(w, p->design); break;
	case A_VOLT:     wl(w, p->voltage_now); break;
	case A_CURR:     wl(w, p->current_now); break;
	}
}

/* uevent: POWER_SUPPLY_<FILE>=value for every other file, as Linux has it */
static void uevent(w_t *w, const ps_t *p)
{
	ws(w, "POWER_SUPPLY_NAME="); ws(w, p->name); ws(w, "\n");
	for (int a = 0; a < A_N; a++) {
		if (a == A_UEVENT || !has(p, a)) continue;
		ws(w, "POWER_SUPPLY_");
		for (const char *s = name_of(p, a); *s; s++) {
			char c[2] = { (char)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s), 0 };
			ws(w, c);
		}
		ws(w, "=");
		value(w, p, a);
		ws(w, "\n");
	}
}

/* A file by its slot: the same slot names the same file whatever else changed */
int ps_attr_slot(const ps_t *p, const char *attr)
{
	for (int a = 0; a < A_N; a++)
		if (has(p, a) && !strcmp(name_of(p, a), attr)) return a;
	return -1;
}

int ps_attr_format_slot(const ps_t *p, int a, char *out, size_t cap)
{
	if (a < 0 || a >= A_N || !has(p, a) || !cap) return -1;
	w_t w = { out, cap, 0 };
	if (a == A_UEVENT) uevent(&w, p);
	else { value(&w, p, a); ws(&w, "\n"); }
	size_t n = w.n < cap ? w.n : cap - 1;
	out[n] = 0;
	return (int)n;
}

int ps_attr_format(const ps_t *p, const char *attr, char *out, size_t cap)
{
	return ps_attr_format_slot(p, ps_attr_slot(p, attr), out, cap);
}

/* The "power supply" line of /proc/hwdiag (fjord doctor), from the supplies the
 * drivers read, no newline. None read: available=0, as before. */
int ps_diag_line(const ps_t *ps, int n, char *out, size_t cap)
{
	if (!cap) return -1;
	w_t w = { out, cap, 0 };
	int mains = 0, online = 0, bats = 0;
	const ps_t *b = NULL;
	for (int i = 0; i < n; i++) {
		if (ps[i].type == PS_MAINS) { mains++; online |= ps[i].online != 0; }
		else if (ps[i].type == PS_BATTERY && ps[i].present) { bats++; if (!b) b = &ps[i]; }
	}
	ws(&w, "power supply available=");
	if (!mains && !bats && !n) {
		ws(&w, "0 reason=no_acpi_aml");
	} else {
		ws(&w, "1");
		if (mains) { ws(&w, " ac_online="); wl(&w, online); }
		ws(&w, " batteries="); wl(&w, bats);
		if (b) {
			static const char *const ST[] = { "unknown", "charging", "discharging", "not_charging", "full" };
			if (b->capacity >= 0) { ws(&w, " capacity="); wl(&w, b->capacity); }
			ws(&w, " status=");
			ws(&w, b->status >= 0 && b->status <= PS_ST_FULL ? ST[b->status] : "unknown");
		}
	}
	size_t k = w.n < cap ? w.n : cap - 1;
	out[k] = 0;
	return (int)k;
}
