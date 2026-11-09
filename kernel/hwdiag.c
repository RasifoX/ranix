/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/hwdiag.h>
#include <kernel/sync.h>
#include <kernel/extable.h>
#include <kernel/lib/string.h>

/*
 * Hardware diagnostics registry behind /proc/hwdiag, plus the CPU readings
 * that need no driver: digital thermal sensor, thermal and power-limit
 * throttling logs (Intel SDM vol. 3, "Thermal Monitoring and Protection",
 * IA32_THERM_STATUS 0x19C, IA32_PACKAGE_THERM_STATUS 0x1B1,
 * MSR_TEMPERATURE_TARGET 0x1A2). The power supply (AC, battery) is described
 * by ACPI AML methods, which Ranix does not interpret, so it is reported as
 * unavailable rather than guessed.
 */

static hwdiag_fn   g_fns[HWDIAG_MAX];
static int         g_nfns;
static spinlock_t  g_lock = SPINLOCK_INIT;

static void hw_char(hw_out_t *o, char c)
{
	if (o->rem > 1) { *o->p++ = c; o->rem--; }
}

static void hw_str(hw_out_t *o, const char *s)
{
	while (*s) hw_char(o, *s++);
}

static void hw_u64(hw_out_t *o, uint64_t v)
{
	char t[21]; int n = 0;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n) hw_char(o, t[--n]);
}

void hw_begin(hw_out_t *o, const char *cls, const char *name)
{
	hw_str(o, cls); hw_char(o, ' '); hw_str(o, name);
}

void hw_kv(hw_out_t *o, const char *key, uint64_t v)
{
	hw_char(o, ' '); hw_str(o, key); hw_char(o, '='); hw_u64(o, v);
}

void hw_ks(hw_out_t *o, const char *key, const char *s)
{
	hw_char(o, ' '); hw_str(o, key); hw_char(o, '=');
	for (; *s; s++) hw_char(o, (*s == ' ' || *s == '\n' || *s == '=') ? '_' : *s);
}

void hw_end(hw_out_t *o) { hw_char(o, '\n'); }

void hwdiag_register(hwdiag_fn fn)
{
	uint64_t f = spin_lock_irq(&g_lock);
	if (g_nfns < HWDIAG_MAX) g_fns[g_nfns++] = fn;
	spin_unlock_irq(&g_lock, f);
}

#if defined(__x86_64__)
static void power_diag(hw_out_t *o);

static void cpuid4(uint32_t leaf, uint32_t r[4])
{
	__asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
	                 : "a"(leaf), "c"(0));
}

/* rdmsr that returns -1 instead of panicking when the MSR does not exist. */
static int rdmsr_safe(uint32_t msr, uint64_t *v)
{
	uint32_t lo = 0, hi = 0;
	int err;
	__asm__ volatile("1: rdmsr\n"
	                 "   xor %[e], %[e]\n"
	                 "   jmp 3f\n"
	                 "2: mov $1, %[e]\n"
	                 "3:\n"
	                 _ASM_EXTABLE("1b", "2b")
	                 : "=a"(lo), "=d"(hi), [e] "=r"(err) : "c"(msr) : "memory");
	if (err) return -1;
	*v = ((uint64_t)hi << 32) | lo;
	return 0;
}

#define MSR_THERM_STATUS      0x19C
#define MSR_RAPL_UNIT         0x606
#define MSR_PKG_ENERGY        0x611
#define MSR_PP0_ENERGY        0x639
#define MSR_PP1_ENERGY        0x641
#define MSR_DRAM_ENERGY       0x619
#define MSR_TEMP_TARGET       0x1A2
#define MSR_PKG_THERM_STATUS  0x1B1

static void cpu_fill(hw_out_t *o)
{
	uint32_t r[4];
	cpuid4(0, r);
	int intel = r[1] == 0x756E6547 && r[3] == 0x49656E69 && r[2] == 0x6C65746E;
	uint32_t a6 = 0;
	if (intel && r[0] >= 6) { cpuid4(6, r); a6 = r[0]; }

	hw_begin(o, "thermal", "cpu");
	if (!(a6 & 1)) {                         /* CPUID.06H:EAX[0] digital sensor */
		hw_kv(o, "available", 0);
		hw_ks(o, "reason", intel ? "no_digital_sensor" : "not_intel");
		hw_end(o);
		hw_begin(o, "power", "supply");
		hw_kv(o, "available", 0);
		hw_ks(o, "reason", "no_acpi_aml");
		hw_end(o);
		return;
	}
	uint64_t tt = 0, ts = 0, ps = 0;
	int have_tj = rdmsr_safe(MSR_TEMP_TARGET, &tt) == 0 && ((tt >> 16) & 0xFF);
	int have_ts = rdmsr_safe(MSR_THERM_STATUS, &ts) == 0 && (ts & (1ull << 31));
	int have_ps = (a6 & (1u << 6)) && rdmsr_safe(MSR_PKG_THERM_STATUS, &ps) == 0;
	uint32_t tj = have_tj ? (uint32_t)((tt >> 16) & 0xFF) : 0;

	hw_kv(o, "available", have_ts || have_ps);
	if (have_tj) hw_kv(o, "tjmax_c", tj);
	if (have_ts) {
		uint32_t below = (uint32_t)((ts >> 16) & 0x7F);
		hw_kv(o, "core_below_tjmax_c", below);
		if (have_tj && below <= tj) hw_kv(o, "core_c", tj - below);
		hw_kv(o, "throttle_now", ts & 1);
		hw_kv(o, "throttle_log", (ts >> 1) & 1);
		hw_kv(o, "prochot_log", (ts >> 3) & 1);
		hw_kv(o, "critical_log", (ts >> 5) & 1);
	}
	if (have_ps) {
		uint32_t below = (uint32_t)((ps >> 16) & 0x7F);
		hw_kv(o, "pkg_below_tjmax_c", below);
		if (have_tj && below <= tj) hw_kv(o, "pkg_c", tj - below);
		hw_kv(o, "pkg_throttle_log", (ps >> 1) & 1);
	}
	hw_end(o);

	hw_begin(o, "power", "cpu");
	int pln = (a6 >> 4) & 1;                 /* CPUID.06H:EAX[4] power limit notify */
	hw_kv(o, "available", pln && (have_ts || have_ps));
	if (pln && have_ts) {
		hw_kv(o, "limit_now", (ts >> 10) & 1);
		hw_kv(o, "limit_log", (ts >> 11) & 1);
	}
	if (pln && have_ps) hw_kv(o, "pkg_limit_log", (ps >> 11) & 1);
	hw_end(o);
	hw_begin(o, "power", "supply");
	hw_kv(o, "available", 0);
	hw_ks(o, "reason", "no_acpi_aml");
	hw_end(o);
	power_diag(o);
}
#else
static void cpu_fill(hw_out_t *o)
{
	hw_begin(o, "thermal", "cpu"); hw_kv(o, "available", 0);
	hw_ks(o, "reason", "not_implemented_on_arch"); hw_end(o);
	hw_begin(o, "power", "supply"); hw_kv(o, "available", 0);
	hw_ks(o, "reason", "not_implemented_on_arch"); hw_end(o);
}
#endif

int hwdiag_fill(char *buf, size_t sz)
{
	hw_out_t o = { buf, sz };
	hw_str(&o, "hwdiag 1\n");
	cpu_fill(&o);
	uint64_t f = spin_lock_irq(&g_lock);
	int n = g_nfns;
	spin_unlock_irq(&g_lock, f);
	for (int i = 0; i < n; i++) g_fns[i](&o);
	return (int)(sz - o.rem);
}

#if defined(__x86_64__)
/* What the package has burned, from its own counters: microjoules, wrapping. Two reads
 * and a clock give watts with no meter on the wall. */
static void power_diag(hw_out_t *o)
{
	uint64_t u, v;
	if (rdmsr_safe(MSR_RAPL_UNIT, &u) != 0) return;
	uint32_t esu = (u >> 8) & 0x1F;
	hw_begin(o, "power", "rapl");
	static const struct { const char *k; uint32_t msr; } C[] = {
		{"package_uj", MSR_PKG_ENERGY}, {"cores_uj", MSR_PP0_ENERGY},
		{"graphics_uj", MSR_PP1_ENERGY}, {"dram_uj", MSR_DRAM_ENERGY},
	};
	for (unsigned i = 0; i < sizeof C / sizeof C[0]; i++)
		if (rdmsr_safe(C[i].msr, &v) == 0)
			hw_kv(o, C[i].k, ((v & 0xFFFFFFFFu) * 1000000u) >> esu);
	hw_end(o);
}
#endif
