/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <uapi/types.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>
#include <kernel/smp.h>
#include <asm/mmu.h>
#include <asm/io.h>
#include <kernel/proc.h>
#include <kernel/board.h>
#include <kernel/hwdiag.h>
#include <kernel/time.h>
#include <asm/cpu.h>
#include <kernel/hal.h>
#include <kernel/errno.h>

/* ── CMOS / RTC ("BIOS clock") → Unix epoch seconds ───────────── */
static uint8_t cmos_read(uint8_t reg) { outb(0x70, reg); return inb(0x71); }
static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v & 0x0F) + (v >> 4) * 10); }

static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int64_t)doe - 719468;
}

int64_t arch_rtc_read(void)
{
	for (int i = 0; i < 1000000; i++) { outb(0x70, 0x0A); if (!(inb(0x71) & 0x80)) break; }
	uint8_t s = cmos_read(0x00), mi = cmos_read(0x02), h = cmos_read(0x04);
	uint8_t d = cmos_read(0x07), mo = cmos_read(0x08), y = cmos_read(0x09);
	uint8_t cent = cmos_read(0x32);
	uint8_t regb = cmos_read(0x0B);
	if (!(regb & 0x04)) { /* BCD → binary */
		s = bcd2bin(s); mi = bcd2bin(mi);
		h = (uint8_t)(((h & 0x0F) + ((h & 0x70) >> 4) * 10) | (h & 0x80));
		d = bcd2bin(d); mo = bcd2bin(mo); y = bcd2bin(y); cent = bcd2bin(cent);
	}
	if (!(regb & 0x02) && (h & 0x80)) h = (uint8_t)(((h & 0x7F) % 12) + 12); /* 12h PM */
	else if (!(regb & 0x02))          h = (uint8_t)(h % 12);
	int64_t year = (cent >= 19 && cent <= 21) ? (int64_t)cent * 100 + y : 2000 + y;
	if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
	int64_t days = days_from_civil(year, mo, d);
	return days * 86400 + (int64_t)h * 3600 + (int64_t)mi * 60 + s;
}

static uint8_t bin2bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static void cmos_write(uint8_t reg, uint8_t val) { outb(0x70, reg); outb(0x71, val); }

/* persist epoch seconds to the CMOS RTC (hwclock --systohc) */
void arch_rtc_write(int64_t epoch)
{
	if (epoch <= 0) return;
	int64_t days = epoch / 86400, rem = epoch % 86400;
	unsigned hh = (unsigned)(rem / 3600), mm = (unsigned)((rem % 3600) / 60), ss = (unsigned)(rem % 60);
	/* civil_from_days (inverse of days_from_civil) */
	int64_t z = days + 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	unsigned doe = (unsigned)(z - era * 146097);
	unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t y = (int64_t)yoe + era * 400;
	unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	unsigned mp = (5 * doy + 2) / 153;
	unsigned dd = doy - (153 * mp + 2) / 5 + 1;
	unsigned mo = mp < 10 ? mp + 3 : mp - 9;
	y += (mo <= 2);

	uint8_t regb = cmos_read(0x0B);
	int bcd = !(regb & 0x04);
	cmos_write(0x0B, (uint8_t)(regb | 0x80));   /* SET: halt updates during write */
	cmos_write(0x00, bcd ? bin2bcd(ss) : (uint8_t)ss);
	cmos_write(0x02, bcd ? bin2bcd(mm) : (uint8_t)mm);
	cmos_write(0x04, bcd ? bin2bcd(hh) : (uint8_t)hh);
	cmos_write(0x07, bcd ? bin2bcd(dd) : (uint8_t)dd);
	cmos_write(0x08, bcd ? bin2bcd(mo) : (uint8_t)mo);
	cmos_write(0x09, bcd ? bin2bcd((unsigned)(y % 100)) : (uint8_t)(y % 100));
	cmos_write(0x32, bcd ? bin2bcd((unsigned)(y / 100)) : (uint8_t)(y / 100));
	cmos_write(0x0B, regb);                     /* clear SET: resume */
}

struct rsdp {
	char     signature[8];      /* "RSD PTR " */
	uint8_t  checksum;
	char     oemid[6];
	uint8_t  revision;          /* 0 = ACPI 1.0, >=2 = ACPI 2.0+ */
	uint32_t rsdt_addr;
	uint32_t length;
	uint64_t xsdt_addr;
	uint8_t  ext_checksum;
	uint8_t  reserved[3];
} __attribute__((packed));

struct sdt_header {
	char     signature[4];
	uint32_t length;
	uint8_t  revision;
	uint8_t  checksum;
	char     oemid[6];
	char     oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} __attribute__((packed));

struct madt {
	struct sdt_header h;
	uint32_t local_apic_addr;
	uint32_t flags;
	uint8_t  entries[];
} __attribute__((packed));

#define MADT_LAPIC    0   /* Processor Local APIC */
#define MADT_X2APIC   9   /* Processor Local x2APIC */

uint64_t mb2_get_rsdp(void);

/* APIC IDs of detected CPUs, logical index -> physical APIC id. Index 0 is the
 * BSP. Filled by acpi_detect_cpus; arch_smp_boot_cpu/IPI use these as the LAPIC
 * destination rather than assuming apic_id == logical index. */
uint32_t cpu_apic_id[MAX_CPUS];
static int acpi_ncpus = 0;

static uint8_t sum_bytes(const void *p, int n)
{
	const uint8_t *b = p;
	uint8_t s = 0;
	for (int i = 0; i < n; i++) s += b[i];
	return s;
}

static struct rsdp *find_rsdp(void)
{
	uint64_t phys = mb2_get_rsdp();
	if (phys) {
		struct rsdp *r = (struct rsdp *)PHYS_TO_VIRT(phys);
		if (memcmp(r->signature, "RSD PTR ", 8) == 0)
			return r;
	}

	/* Fallback: scan the EBDA (first 1 KB) and the BIOS ROM area on 16-byte
	 * boundaries — the legacy locations the spec mandates. */
	uint64_t ebda = (uint64_t)(*(uint16_t *)PHYS_TO_VIRT(0x40EUL)) << 4;
	uint64_t ranges[2][2] = { { ebda, ebda + 1024 }, { 0xE0000, 0x100000 } };
	for (int rg = 0; rg < 2; rg++) {
		if (ranges[rg][0] == 0) continue;
		for (uint64_t a = ranges[rg][0]; a < ranges[rg][1]; a += 16) {
			struct rsdp *r = (struct rsdp *)PHYS_TO_VIRT(a);
			if (memcmp(r->signature, "RSD PTR ", 8) == 0 &&
			    sum_bytes(r, 20) == 0)
				return r;
		}
	}
	return 0;
}

static void parse_madt(struct madt *m)
{
	uint32_t off = sizeof(struct sdt_header) + 8;   /* skip lapic_addr + flags */
	while (off + 2 <= m->h.length) {
		uint8_t *e = (uint8_t *)m + off;
		uint8_t type = e[0], len = e[1];
		if (len < 2) break;

		if (type == MADT_LAPIC) {
			uint8_t  apic_id = e[3];
			uint32_t flags   = *(uint32_t *)(e + 4);
			if ((flags & 1) && acpi_ncpus < (int)(sizeof(cpu_apic_id) / sizeof(cpu_apic_id[0])))
				cpu_apic_id[acpi_ncpus++] = apic_id;
		} else if (type == MADT_X2APIC) {
			uint32_t x2id  = *(uint32_t *)(e + 4);
			uint32_t flags = *(uint32_t *)(e + 8);
			if ((flags & 1) && acpi_ncpus < (int)(sizeof(cpu_apic_id) / sizeof(cpu_apic_id[0])))
				cpu_apic_id[acpi_ncpus++] = x2id;
		}
		off += len;
	}
}

static void *find_table(struct rsdp *r, const char *sig)
{
	int use_xsdt = (r->revision >= 2 && r->xsdt_addr);
	struct sdt_header *root =
		(struct sdt_header *)PHYS_TO_VIRT(use_xsdt ? r->xsdt_addr : r->rsdt_addr);

	int n = (root->length - sizeof(struct sdt_header)) / (use_xsdt ? 8 : 4);
	uint8_t *ptrs = (uint8_t *)root + sizeof(struct sdt_header);

	for (int i = 0; i < n; i++) {
		uint64_t pa;
		if (use_xsdt) memcpy(&pa, ptrs + i * 8, 8);
		else { uint32_t p32; memcpy(&p32, ptrs + i * 4, 4); pa = p32; }

		struct sdt_header *t = (struct sdt_header *)PHYS_TO_VIRT(pa);
		if (memcmp(t->signature, sig, 4) == 0)
			return t;
	}
	return 0;
}

/* ── HPET: high-resolution monotonic counter (a Linux clocksource) ──────────
 * The HPET ACPI table gives an MMIO base; CAP[63:32] is the tick period in
 * femtoseconds, MAIN_COUNTER (0xF0) a free-running 64-bit count. We enable it
 * and expose hpet_now_ns() so x86 clock_gettime/mono_time get ns resolution
 * instead of the 10 ms tick (aarch64 already has cntvct). */
static volatile uint8_t *hpet_base;
static uint64_t          hpet_period_fs;

static void hpet_init_from(struct rsdp *r)
{
	void *t = find_table(r, "HPET");
	if (!t) return;
	uint64_t base = *(uint64_t *)((uint8_t *)t + 44);   /* GAS address @ offset 44 */
	if (!base) return;
	hpet_base = (volatile uint8_t *)PHYS_TO_VIRT(base);
	hpet_period_fs = (*(volatile uint64_t *)hpet_base) >> 32;
	if (!hpet_period_fs || hpet_period_fs > 100000000ULL) { hpet_base = 0; return; }
	*(volatile uint64_t *)(hpet_base + 0x10) |= 1;      /* ENABLE_CNF */
}

uint64_t hpet_now_ns(void)
{
	if (!hpet_base) return 0;
	uint64_t cnt = *(volatile uint64_t *)(hpet_base + 0xF0);
	/* ns = cnt * period_fs / 1e6, split to avoid 64-bit overflow */
	return (cnt / 1000000ULL) * hpet_period_fs + ((cnt % 1000000ULL) * hpet_period_fs) / 1000000ULL;
}

/* The TSC, timed against the HPET once. Reading the HPET is an uncached bus cycle
 * and a syscall around it; a browser asks the time thousands of times a second.
 * Only an invariant TSC qualifies: it ticks at one rate in every power state. */
uint64_t tsc_mult;                      /* ns = delta * mult >> 28 */
static uint64_t tsc0, tsc_ns0;

static inline uint64_t rdtsc_(void)
{
	uint32_t lo, hi;
	__asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi));
	return (uint64_t)hi << 32 | lo;
}

void tsc_calibrate(void)
{
	uint32_t a, d;
	__asm__ volatile("cpuid" : "=a"(a), "=d"(d) : "a"(0x80000007U) : "ebx", "ecx");
	if (!(d & (1U << 8)) || !hpet_base)
		return;
	uint64_t h0 = hpet_now_ns(), t0 = rdtsc_(), h1;
	do { h1 = hpet_now_ns(); } while (h1 - h0 < 20000000ULL);
	uint64_t t1 = rdtsc_();
	if (t1 <= t0)
		return;
	tsc0 = t1;
	tsc_ns0 = h1;
	uint64_t mult = ((h1 - h0) << 28) / (t1 - t0);
	/* The CPU can say its TSC rate exactly: crystal times a ratio (CPUID 15h).
	 * 20 ms against the HPET came out 0.28 % off on Casper, ten seconds an hour.
	 * The crystal is left out on Sky Lake and Kaby Lake clients: 24 MHz there. */
	uint32_t eax, ebx, ecx, edx, max;
	__asm__ volatile("cpuid" : "=a"(max), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
	if (max >= 0x15) {
		__asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x15), "c"(0));
		uint64_t crystal = ecx;
		if (!crystal) {
			uint32_t a1, b1, c1, d1;
			__asm__ volatile("cpuid" : "=a"(a1), "=b"(b1), "=c"(c1), "=d"(d1) : "a"(1));
			uint32_t fam = (a1 >> 8) & 15, model = ((a1 >> 4) & 15) | ((a1 >> 12) & 0xF0);
			if (fam == 6 && (model == 0x4E || model == 0x5E || model == 0x8E || model == 0x9E ||
			                 model == 0xA5 || model == 0xA6))
				crystal = 24000000;
		}
		if (eax && ebx && crystal) {
			uint64_t hz = crystal * ebx / eax;
			uint64_t exact = (1000000000ULL << 28) / hz;
			/* trust it only near the measurement: a hypervisor may report junk */
			if (exact > mult - mult / 50 && exact < mult + mult / 50) {
				kprintf(KERN_INFO "tsc: %lu Hz from cpuid (measured %lu)\n", (unsigned long)hz,
				        (unsigned long)((1000000000ULL << 28) / mult));
				mult = exact;
			}
		}
	}
	__sync_synchronize();
	tsc_mult = mult;
}

uint64_t tsc_now_ns(void)
{
	if (!tsc_mult)
		return 0;
	uint64_t delta = rdtsc_() - tsc0;
	return tsc_ns0 + (uint64_t)(((unsigned __int128)delta * tsc_mult) >> 28);
}

/* ── FADT reset register ────────────────────────────────────────────────────
 * Parsed at boot, not at panic: the panic path must not walk firmware tables. */
#define FADT_FLAGS        112
#define FADT_RESET_REG    116     /* generic address structure, 12 bytes */
#define FADT_RESET_VALUE  128
#define FADT_RESET_SUP    (1u << 10)
#define GAS_SPACE_MEM     0
#define GAS_SPACE_IO      1

static int      fadt_reset_ok;
static uint8_t  fadt_reset_space, fadt_reset_val;
static uint64_t fadt_reset_addr;

static void fadt_reset_init(struct rsdp *r)
{
	uint8_t *f = find_table(r, "FACP");
	if (!f || ((struct sdt_header *)f)->length <= FADT_RESET_VALUE) return;

	uint32_t flags;
	memcpy(&flags, f + FADT_FLAGS, sizeof(flags));
	if (!(flags & FADT_RESET_SUP)) return;

	uint64_t addr;
	memcpy(&addr, f + FADT_RESET_REG + 4, sizeof(addr));
	uint8_t space = f[FADT_RESET_REG];
	if (!addr || (space != GAS_SPACE_IO && space != GAS_SPACE_MEM)) {
		kprintf(KERN_WARN "acpi: reset register in unsupported space %d\n", space);
		return;
	}
	fadt_reset_space = space;
	fadt_reset_addr  = addr;
	fadt_reset_val   = f[FADT_RESET_VALUE];
	fadt_reset_ok    = 1;
	kprintf(KERN_DEBUG "acpi: reset register %s 0x%lx value 0x%x\n",
	        space == GAS_SPACE_IO ? "io" : "mem", addr, fadt_reset_val);
}

/* ── S5: soft off ──────────────────────────────
 * The sleep type for S5 is in the DSDT, in AML: Name (_S5_, Package () {a, b, ...}).
 * A real AML interpreter is not needed to read one constant package. */
static uint16_t pm1a_cnt, pm1b_cnt, slp_typa, slp_typb;
static uint32_t smi_cmd;
static uint8_t  acpi_enable_val;
static int      s5_ok;
static uint16_t pm1a_evt;
static uint8_t  pm1_len;

static int aml_byte(const uint8_t **p, uint8_t *out)
{
	const uint8_t *q = *p;
	if (*q == 0x0A) { *out = q[1]; *p = q + 2; return 1; }   /* BytePrefix */
	if (*q == 0x00 || *q == 0x01) { *out = *q; *p = q + 1; return 1; }   /* Zero, One */
	return 0;
}

static void s5_init(struct rsdp *r)
{
	uint8_t *f = find_table(r, "FACP");
	if (!f || ((struct sdt_header *)f)->length < 76) return;
	uint32_t dsdt, a, b, smi, evt;
	memcpy(&evt,  f + 56, 4);
	if (((struct sdt_header *)f)->length > 88 && evt && evt <= 0xFFFF) {
		pm1a_evt = (uint16_t)evt;
		pm1_len = f[88];
	}
	memcpy(&dsdt, f + 40, 4);
	memcpy(&smi,  f + 48, 4);
	memcpy(&a,    f + 64, 4);
	memcpy(&b,    f + 68, 4);
	acpi_enable_val = f[52];
	if (!dsdt || !a || a > 0xFFFF || b > 0xFFFF) return;
	struct sdt_header *h = (struct sdt_header *)PHYS_TO_VIRT((uint64_t)dsdt);
	uint32_t len = h->length;
	const uint8_t *d = (const uint8_t *)h;
	for (uint32_t i = sizeof(*h); i + 12 < len; i++) {
		if (memcmp(d + i, "_S5_", 4) != 0) continue;
		const uint8_t *p = d + i + 4;
		if (*p++ != 0x12) continue;              /* PackageOp */
		p += (*p >> 6) + 1;                      /* PkgLength */
		p++;                                     /* NumElements */
		uint8_t ta, tb;
		if (!aml_byte(&p, &ta) || !aml_byte(&p, &tb)) continue;
		pm1a_cnt = (uint16_t)a;
		pm1b_cnt = (uint16_t)b;
		slp_typa = ta;
		slp_typb = tb;
		smi_cmd  = smi;
		s5_ok    = 1;
		kprintf(KERN_INFO "acpi: S5 sleep type %u/%u, PM1 control 0x%x\n", ta, tb, a);
		return;
	}
}

static uint16_t inw_port(uint16_t port)
{
	uint16_t v;
	__asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static void outw_port(uint16_t port, uint16_t v)
{
	__asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(port));
}

/* Returns only if the machine did not go off */
void acpi_poweroff(void)
{
	if (!s5_ok) return;
	if (!(inw_port(pm1a_cnt) & 1) && smi_cmd && acpi_enable_val) {   /* SCI_EN clear: legacy */
		__asm__ volatile("outb %0, %1" :: "a"(acpi_enable_val), "Nd"((uint16_t)smi_cmd));
		for (int i = 0; i < 3000000 && !(inw_port(pm1a_cnt) & 1); i++) ;
	}
	/* the type first, then the enable: some chipsets take them only in two writes */
	uint16_t a = (uint16_t)((inw_port(pm1a_cnt) & ~0x3C00u) | (slp_typa << 10));
	outw_port(pm1a_cnt, a);
	if (pm1b_cnt) {
		uint16_t b = (uint16_t)((inw_port(pm1b_cnt) & ~0x3C00u) | (slp_typb << 10));
		outw_port(pm1b_cnt, b);
		outw_port(pm1b_cnt, (uint16_t)(b | 0x2000u));
	}
	outw_port(pm1a_cnt, (uint16_t)(a | 0x2000u));
	for (int i = 0; i < 30000000; i++) __asm__ volatile("pause");
}

/* The power button is a status bit in PM1. Polled: an SCI would need the interrupt
 * routing that lives in AML. Pressed once, the system is told to shut down in order. */
#define PM1_PWRBTN 0x0100u

static void acpi_button_thread(void)
{
	for (;;) {
		task_sleep_until(kernel_ticks + TIMER_HZ / 4);
		if (!(inw_port(pm1a_evt) & PM1_PWRBTN)) continue;
		outw_port(pm1a_evt, PM1_PWRBTN);
		kprintf(KERN_INFO "acpi: power button\n");
		process_t *init = proc_get(1);
		if (init) {
			task_signal(init, 30);     /* SIGPWR */
			proc_put(init);
		}
	}
}

static void acpi_diag(hw_out_t *o)
{
	hw_begin(o, "power", "acpi");
	hw_kv(o, "s5", s5_ok);
	hw_kv(o, "sci_en", s5_ok ? inw_port(pm1a_cnt) & 1 : 0);
	hw_kv(o, "smi_cmd", smi_cmd);
	hw_kv(o, "acpi_enable", acpi_enable_val);
	hw_kv(o, "pm1_sts", inw_port(pm1a_evt));
	hw_kv(o, "pm1_en", inw_port((uint16_t)(pm1a_evt + pm1_len / 2)));
	hw_end(o);
}

void acpi_button_start(void)
{
	if (!pm1a_evt) return;
	outw_port(pm1a_evt, PM1_PWRBTN);       /* a press from before does not count */
	hwdiag_register(acpi_diag);
	proc_create("acpi_button", acpi_button_thread);
}

void acpi_reset(void)
{
	if (!fadt_reset_ok) return;
	if (fadt_reset_space == GAS_SPACE_IO)
		outb((uint16_t)fadt_reset_addr, fadt_reset_val);
	else
		*(volatile uint8_t *)PHYS_TO_VIRT(fadt_reset_addr) = fadt_reset_val;
}

/* ── Firmware: the global lock and the DSDT's name ─────────────────────────
 * AML fields declared "Lock" are shared with firmware (SMM): whoever touches them
 * holds the global lock, a dword in the FACS (bit 1 owned, bit 0 pending). Ranix
 * takes it only when it is free and never sets pending, so it needs no SCI. If
 * firmware set pending while Ranix held the lock, GBL_RLS in PM1 control tells
 * it the lock is free again (ACPI 6.4, 5.2.10.1). */
#define FADT_FIRMWARE_CTRL   36
#define FADT_PM1A_CNT        64
#define FADT_X_FIRMWARE_CTRL 132
#define FACS_GLOBAL_LOCK     16
#define GL_PENDING           1u
#define GL_OWNED             2u
#define PM1_GBL_RLS          0x0004u
#define PM1_SLP_EN           0x2000u

static volatile uint32_t *fw_glock;
static uint16_t fw_pm1a_cnt;
static char fw_oem[7], fw_table[9];

static void fw_init(struct rsdp *r)
{
	uint8_t *f = find_table(r, "FACP");
	if (!f) return;
	uint32_t len = ((struct sdt_header *)f)->length, fc32 = 0, pm1 = 0, dsdt = 0;
	uint64_t fc = 0;
	memcpy(&fc32, f + FADT_FIRMWARE_CTRL, 4);
	memcpy(&pm1, f + FADT_PM1A_CNT, 4);
	memcpy(&dsdt, f + 40, 4);
	if (len >= FADT_X_FIRMWARE_CTRL + 8) memcpy(&fc, f + FADT_X_FIRMWARE_CTRL, 8);
	if (!fc) fc = fc32;
	if (fc && pm1 && pm1 <= 0xFFFF) {
		uint8_t *facs = (uint8_t *)PHYS_TO_VIRT(fc);
		uint32_t flen;
		memcpy(&flen, facs + 4, 4);
		if (!memcmp(facs, "FACS", 4) && flen >= FACS_GLOBAL_LOCK + 4) {
			fw_glock = (volatile uint32_t *)(facs + FACS_GLOBAL_LOCK);
			fw_pm1a_cnt = (uint16_t)pm1;
		}
	}
	if (dsdt) {
		struct sdt_header *h = (struct sdt_header *)PHYS_TO_VIRT((uint64_t)dsdt);
		if (!memcmp(h->signature, "DSDT", 4)) {
			memcpy(fw_oem, h->oemid, 6);
			memcpy(fw_table, h->oem_table_id, 8);
		}
	}
}

int arch_fw_lock(uint32_t timeout_ms)
{
	if (!fw_glock) return 0;
	uint64_t until = kernel_ticks + (uint64_t)timeout_ms * TIMER_HZ / 1000 + 1;
	for (;;) {
		uint32_t old = *fw_glock;
		if (!(old & GL_OWNED) &&
		    __sync_bool_compare_and_swap(fw_glock, old, (old & ~GL_PENDING) | GL_OWNED))
			return 0;
		if (kernel_ticks >= until) return -EBUSY;
		task_sleep_until(kernel_ticks + 1);
	}
}

void arch_fw_unlock(void)
{
	if (!fw_glock) return;
	uint32_t old;
	do old = *fw_glock;
	while (!__sync_bool_compare_and_swap(fw_glock, old, old & ~(GL_OWNED | GL_PENDING)));
	if (old & GL_PENDING)
		outw_port(fw_pm1a_cnt, (uint16_t)((inw_port(fw_pm1a_cnt) & ~PM1_SLP_EN) | PM1_GBL_RLS));
}

int arch_fw_oem(char oem[7], char table[9])
{
	memcpy(oem, fw_oem, 7);
	memcpy(table, fw_table, 9);
	return fw_oem[0] ? 0 : -ENODEV;
}

/* Returns the number of enabled CPUs from the ACPI MADT, 0 if no MADT. */
int acpi_detect_cpus(void)
{
	if (acpi_ncpus) return acpi_ncpus;

	struct rsdp *r = find_rsdp();
	if (!r) return 0;

	hpet_init_from(r);   /* independent of the MADT */
	fadt_reset_init(r);
	s5_init(r);
	fw_init(r);

	struct madt *m = find_table(r, "APIC");
	if (!m) return 0;

	parse_madt(m);
	return acpi_ncpus;
}
