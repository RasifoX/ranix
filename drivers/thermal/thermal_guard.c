/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * Ranix thermal guard: keep the package below its trip point without ACPI.
 *
 * Ranix does not run the ACPI thermal zones or the fan yet, so under load the
 * package can heat until the platform cuts power (seen on Casper, 2026-11-08).
 * Every 250 ms (from the timer tick on CPU0) this reads the package temperature
 * (IA32_PACKAGE_THERM_STATUS 0x1B1 against TjMax in MSR_TEMPERATURE_TARGET 0x1A2)
 * and limits the whole package through RAPL (MSR_PKG_POWER_LIMIT 0x610, PL1 and
 * PL2; package scope, so one write from CPU0 covers every core):
 *
 *   >= 78 C  step the limit down by 2 W per 250 ms, to no lower than 5 W
 *   >= 92 C  emergency: 3 W at once
 *   72-78 C  hold
 *   <  72 C  raise it by 2 W per second; at the firmware's own limit (read at
 *            boot) hand the register back. Stepping up, not jumping back to the
 *            firmware's 25 W, keeps it from oscillating (measured on Casper:
 *            123 step-downs and 17 restores in 3.5 min with the jump).
 *
 * Each step logs one kernel line. /sys/kernel/ranix_thermal (root only) reads the state.
 * Off on anything but bare-metal Intel with a package thermal sensor and RAPL
 * (QEMU sets the hypervisor bit: no MSR reads there), and when the firmware has
 * locked 0x610 (then it only reports).
 */
#include <kernel/vfs.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/tick.h>
#include <kernel/hal.h>
#include <kernel/proc.h>

#define T_STEP_C   78
#define T_EMERG_C  92
#define T_CLEAR_C  72
#define STEP_MW    2000
#define FLOOR_MW   5000
#define EMERG_MW   3000
#define PERIOD     250                 /* ticks (1 kHz) */
#define RAISE_EVERY 4                  /* periods: 2 W up per second */

/* The policy, separate from the MSRs so it can be tested: the next limit in mW
 * (0 = the firmware's) for this temperature. */
uint32_t thermal_next_limit(uint32_t temp, uint32_t limit_mw, uint32_t firmware_mw, uint32_t *calm_periods)
{
	if (temp >= 92) { *calm_periods = 0; return 3000; }
	if (temp >= 78) {
		*calm_periods = 0;
		uint32_t from = limit_mw ? limit_mw : firmware_mw;
		return from > 5000 + 2000 ? from - 2000 : 5000;
	}
	if (temp >= 72 || !limit_mw) { *calm_periods = 0; return limit_mw; }
	if (++*calm_periods < RAISE_EVERY) return limit_mw;
	*calm_periods = 0;
	return limit_mw + 2000 >= firmware_mw ? 0 : limit_mw + 2000;
}

static struct {
	int      on, locked;
	uint32_t tjmax, temp, max_temp;
	uint64_t orig;                     /* 0x610 as the firmware left it */
	uint32_t unit_mw_shift;            /* power unit: 1/2^n W */
	uint32_t orig_pl1_mw, limit_mw;    /* limit_mw 0: not limiting */
	uint32_t steps, emergencies, restores;
	uint32_t count, calm;
} T;

#ifdef __x86_64__
static uint64_t rdmsr_(uint32_t m)
{
	uint32_t lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m));
	return (uint64_t)hi << 32 | lo;
}
static void wrmsr_(uint32_t m, uint64_t v)
{
	__asm__ volatile("wrmsr" :: "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static void cpuid_(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
	__asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static uint32_t mw_to_units(uint32_t mw) { return (uint32_t)(((uint64_t)mw << T.unit_mw_shift) / 1000); }
static uint32_t units_to_mw(uint32_t u)  { return (uint32_t)(((uint64_t)u * 1000) >> T.unit_mw_shift); }

/* PL1 and PL2 both at mw, enabled and clamped; the time windows stay the firmware's */
static void set_limit(uint32_t mw)
{
	uint64_t v = T.orig;
	uint64_t u = mw_to_units(mw) & 0x7fff;
	v &= ~(0x7fffULL | (0x7fffULL << 32));
	v |= u | (1ULL << 15) | (1ULL << 16);              /* PL1, enable, clamp */
	v |= (u << 32) | (1ULL << 47) | (1ULL << 48);      /* PL2, enable, clamp */
	wrmsr_(0x610, v);
	T.limit_mw = mw;
}

static void guard_tick(void)
{
	if (!T.on || ++T.count < PERIOD) return;
	T.count = 0;
	uint32_t below = (uint32_t)(rdmsr_(0x1B1) >> 16) & 0x7f;
	T.temp = T.tjmax > below ? T.tjmax - below : 0;
	if (T.temp > T.max_temp) T.max_temp = T.temp;
	if (T.locked) return;
	uint32_t next = thermal_next_limit(T.temp, T.limit_mw, T.orig_pl1_mw, &T.calm);
	if (next == T.limit_mw) return;
	if (!next) {
		wrmsr_(0x610, T.orig);
		T.limit_mw = 0;
		T.restores++;
		kprintf("thermal: %u C: package limit back to the firmware's (%u W)\n", T.temp, T.orig_pl1_mw / 1000);
		return;
	}
	if (next == 3000 && T.limit_mw != 3000) T.emergencies++;
	else if (next < (T.limit_mw ? T.limit_mw : T.orig_pl1_mw)) T.steps++;
	set_limit(next);
	kprintf("thermal: %u C: package limit %u.%u W\n", T.temp, next / 1000, next % 1000 / 100);
}
#endif

void thermal_guard_init(void)
{
#ifdef __x86_64__
	uint32_t a, b, c, d;
	cpuid_(0, &a, &b, &c, &d);
	if (!(b == 0x756e6547 && d == 0x49656e69 && c == 0x6c65746e)) return;   /* GenuineIntel */
	cpuid_(1, &a, &b, &c, &d);
	if (c & (1u << 31)) return;                         /* under a hypervisor: leave the MSRs alone */
	if (((a >> 8) & 0xf) != 6) return;
	cpuid_(6, &a, &b, &c, &d);
	if (!(a & 1) || !(a & (1u << 6))) return;           /* digital sensor, package thermal */
	T.tjmax = (uint32_t)(rdmsr_(0x1A2) >> 16) & 0xff;
	if (T.tjmax < 70 || T.tjmax > 125) return;
	T.unit_mw_shift = (uint32_t)rdmsr_(0x606) & 0xf;
	T.orig = rdmsr_(0x610);
	T.locked = (T.orig >> 63) & 1;
	T.orig_pl1_mw = units_to_mw((uint32_t)T.orig & 0x7fff);
	if (!T.orig_pl1_mw) T.orig_pl1_mw = 15000;
	T.on = 1;
	tick_register(guard_tick);
	kprintf("thermal: guard on, TjMax %u C, package limit %u W%s; steps at %u C, emergency at %u C, clear below %u C\n",
	        T.tjmax, T.orig_pl1_mw / 1000, T.locked ? " (locked by firmware: report only)" : "",
	        T_STEP_C, T_EMERG_C, T_CLEAR_C);
	(void)STEP_MW; (void)FLOOR_MW; (void)EMERG_MW;
#endif
}

/* ── /sys/kernel/ranix_thermal ── */

static int putu(char *o, int n, const char *k, uint32_t v)
{
	while (*k) o[n++] = *k++;
	char t[12]; int i = 0;
	do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (i) o[n++] = t[--i];
	o[n++] = ' ';
	return n;
}

static ssize_t thermal_read(vfs_file_t *f, void *buf, size_t len)
{
	if (task_current()->euid != 0) return -13;     /* EACCES: root only */
	char o[256];
	int n = 0;
	if (!T.on) {
		const char *s = "off (no package sensor or not bare-metal Intel)\n";
		while (*s) o[n++] = *s++;
	} else {
		n = putu(o, n, "temp_C ", T.temp);
		n = putu(o, n, "max_C ", T.max_temp);
		n = putu(o, n, "tjmax_C ", T.tjmax);
		n = putu(o, n, "limit_mW ", T.limit_mw);
		n = putu(o, n, "firmware_mW ", T.orig_pl1_mw);
		n = putu(o, n, "steps ", T.steps);
		n = putu(o, n, "emergencies ", T.emergencies);
		n = putu(o, n, "restores ", T.restores);
		n = putu(o, n, "locked ", (uint32_t)T.locked);
		o[n - 1] = '\n';
	}
	if (f->pos >= n) return 0;
	size_t c = (size_t)(n - f->pos);
	if (c > len) c = len;
	if (copy_fop_to_user(buf, o + f->pos, c) != 0) return -14;
	f->pos += (off_t)c;
	return (ssize_t)c;
}

static vfs_fops_t thermal_fops = { .read = thermal_read };

vfs_inode_t *thermal_sysfs_file(void)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino = 0xd3000001;
	in->mode = S_IFREG | 0400;               /* root only: diagnostics, nothing opened by default */
	in->nlink = 1;
	in->refcount = 1;
	in->size = 256;
	in->fops = &thermal_fops;
	return in;
}
