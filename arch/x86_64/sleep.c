/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * Suspend on x86_64: the CPUs, the interrupt lines, the RTC alarm, S3.
 * kernel/suspend.c drives it; the design is ozel-SUSP1/TASARIM.md.
 *
 * Parking. Every other CPU takes the 0xF3 IPI, masks its local timer and waits
 * in the handler with IRQs off. An IPI is taken only with IRQs on, so a parked
 * CPU holds no spin_lock_irq; whatever may sleep or lock was done before.
 *   s2idle: it waits (mwait on park_hold) until released.
 *   deep:   it saves itself (sleep_save_cpu) and halts; S3 resets it. After the
 *           resume the BSP sends it INIT-SIPI to the wake page, it comes back
 *           into the handler from sleep_save_cpu and returns from the IPI.
 *
 * s2idle on the BSP: the 8259 passes only the lines that wake (keyboard, RTC,
 * the SCI); the local timer is masked, or fires once a second when there is no
 * SCI to tell of a button press; lapic_timer_irq does no tick work while
 * sleep_frozen. Each wakeup is looked at: a press, an SCI, a key, the alarm's
 * time. Anything else (a device's MSI) is served and the CPU sleeps again.
 *
 * deep (ACPI S3, only with ranix.s3=1): \_S3_ from the DSDT (no AML runs, so
 * _PTS and _WAK do not: on Casper they talk to the EC, see TASARIM.md), the
 * FACS waking vector at the wake page, SLP_TYP then SLP_EN.
 */
#include <uapi/types.h>
#include <asm/cpu.h>
#include <asm/gdt.h>
#include <asm/kvm.h>
#include <asm/io.h>
#include <asm/mmu.h>
#include <asm/sleep.h>
#include <kernel/cmdline.h>
#include <kernel/errno.h>
#include <kernel/hal.h>
#include <kernel/hwdiag.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>
#include <kernel/pci.h>
#include <kernel/proc.h>
#include <kernel/sleep.h>
#include <kernel/smp.h>
#include <kernel/time.h>
#include <drivers/irqchip/pic8259/pic8259.h>
#include <drivers/irqchip/irqchip.h>

void uart16550_init(void);
void uart16550_irq_init(void);
void acpi_ec_gpe_rearm(void);

volatile int sleep_frozen;          /* smp.c: the BSP's timer does no tick work */
volatile int sleep_timer_fired;

static volatile int park_hold;
static volatile int park_state[MAX_CPUS];   /* 0 running, 1 parked, 2 back from S3 */
static int park_kind;
static sleep_cpu_t cpu_save[MAX_CPUS];

static acpi_sleep_info_t SI;
static int si_ok, deep_ok, probed;
static uint8_t s3_a, s3_b;
static const char *deep_why = "not probed";
static uint32_t n_s2idle, n_deep, n_spurious, n_park_fail, n_ap_lost;

/* ---- probing ---- */

static void page_ok(uint64_t base, uint64_t len, void *ud)
{
	if (base <= WAKE_PHYS && base + len >= WAKE_PHYS + 0x1000) *(int *)ud = 1;
}

static void probe(void)
{
	if (probed) return;
	probed = 1;
	if (acpi_sleep_info(&SI)) { deep_why = "no ACPI PM1 blocks"; return; }
	si_ok = 1;
	int usable = 0;
	mb2_foreach_mem_region(page_ok, &usable);
	int r = SI.dsdt ? acpi_sleep_type(SI.dsdt, SI.dsdt_len, 3, &s3_a, &s3_b) : SLEEP_TYPE_NONE;
	char v[8];   /* "force": 4 bytes cut it to "for" and q35's S3 test never slept deep (11-15) */
	if (r == SLEEP_TYPE_NONE)              deep_why = "no \\_S3_ in the DSDT";
	else if (r == SLEEP_TYPE_COND)         deep_why = "\\_S3_ depends on AML Ranix does not run";
	else if (!SI.facs)                     deep_why = "no FACS for the waking vector";
	else if (!usable)                      deep_why = "the wake page is not usable RAM";
	/* code + data on the one wake page; the code's own limit (WAKE_DATA) is the
	 * assembler's .org. The old check compared the whole page with WAKE_DATA and
	 * kept deep off everywhere (SUSP1 KVM 11-13: 0xF56 > 0xF00) */
	else if (sleep_wake_end - sleep_wake_start > 0x1000) deep_why = "wake page longer than a page";
	else if (!(cmdline_get("ranix.s3", v, sizeof v) && (!strcmp(v, "1") || !strcmp(v, "force"))))
		deep_why = "a test feature for now: ranix.s3=1";
	else { deep_ok = 1; deep_why = ""; }
	kprintf(KERN_INFO "sleep: s2idle; deep (S3) %s%s; firmware low-power S0 idle %u\n",
	        deep_ok ? "available" : "off, ", deep_why, (unsigned)((SI.fadt_flags >> 21) & 1));
}

int arch_sleep_deep_ok(void) { probe(); return deep_ok; }
const char *arch_sleep_deep_why(void) { probe(); return deep_why; }

/* ---- the RTC alarm (CMOS registers 1/3/5, B.AIE, C.AF) ----
 * Hours, minutes and seconds: the alarm matches once a day. A time further than
 * a day off makes the loop look at the RTC on each match and sleep on. */
static volatile int64_t alarm_at;
static volatile int rtc_af;
static int rtc_irq_on;

static uint8_t cmos_rd(uint8_t r) { outb(0x70, r); return inb(0x71); }
static void cmos_wr(uint8_t r, uint8_t v) { outb(0x70, r); outb(0x71, v); }
static uint8_t to_bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void rtc_irq(void)
{
	if (cmos_rd(0x0C) & 0x20) rtc_af = 1;      /* reading C acks the line */
}

int rtc_wakealarm_set(int64_t at)
{
	uint64_t f = irq_save();
	uint8_t b = cmos_rd(0x0B);
	if (!at) {
		cmos_wr(0x0B, (uint8_t)(b & ~0x20));
		alarm_at = 0;
		irq_restore(f);
		return 0;
	}
	int64_t rem = at % 86400;
	unsigned hh = (unsigned)(rem / 3600), mm = (unsigned)(rem % 3600 / 60), ss = (unsigned)(rem % 60);
	int bcd = !(b & 0x04), h24 = b & 0x02;
	uint8_t hv;
	if (h24) hv = bcd ? to_bcd(hh) : (uint8_t)hh;
	else {
		unsigned h12 = hh % 12 ? hh % 12 : 12;
		hv = (uint8_t)((bcd ? to_bcd(h12) : h12) | (hh >= 12 ? 0x80 : 0));
	}
	cmos_wr(0x01, bcd ? to_bcd(ss) : (uint8_t)ss);
	cmos_wr(0x03, bcd ? to_bcd(mm) : (uint8_t)mm);
	cmos_wr(0x05, hv);
	(void)cmos_rd(0x0C);
	rtc_af = 0;
	alarm_at = at;
	cmos_wr(0x0B, (uint8_t)(b | 0x20));         /* AIE */
	if (!rtc_irq_on) { irq_register(0x28, rtc_irq); rtc_irq_on = 1; }
	irq_restore(f);
	return 0;
}

int64_t rtc_wakealarm_get(void) { return alarm_at; }

static int alarm_due(void)
{
	return alarm_at && arch_rtc_read() >= alarm_at;
}

/* ---- parking ---- */

static int park_mwait;

static void park_wait(void)
{
	while (park_hold) {
		if (park_mwait) {
			__asm__ volatile("monitor" :: "a"(&park_hold), "c"(0), "d"(0));
			if (!park_hold) break;
			__asm__ volatile("mwait" :: "a"(0), "c"(0));
		} else {
			__asm__ volatile("pause" ::: "memory");
		}
	}
}

/* The TSS, IDT, syscall MSRs, PAT, HWP and the local APIC of a CPU back from S3. */
static void cpu_reinit(int cpu)
{
	vmx_cpu_reset(cpu);   /* S3 took it out of VMX operation */
	gdt_init(cpu);
	idt_load();
	arch_syscall_init();
	arch_pat_init();
	arch_hwp_init();
	lapic_resume_cpu();
}

/* The 0xF3 IPI (exceptions_asm.S), IRQs off. */
void sleep_park_handler(void)
{
	int cpu = (int)arch_get_cpu_ctx()->cpu_id;
	lapic_timer_mask();
	lapic_eoi();
	if (park_kind == SLEEP_DEEP) {
		if (sleep_save_cpu(&cpu_save[cpu]) == 0) {
			park_state[cpu] = 1;
			__asm__ volatile("wbinvd" ::: "memory");
			for (;;) __asm__ volatile("cli; hlt");
		}
		cpu_reinit(cpu);
		park_state[cpu] = 2;
	} else {
		park_state[cpu] = 1;
	}
	park_wait();
	park_state[cpu] = 0;
	lapic_timer_start();
}

static void wake_page(sleep_cpu_t *ctx)
{
	uint8_t *p = (uint8_t *)PHYS_TO_VIRT((uint64_t)WAKE_PHYS);
	memcpy(p, sleep_wake_start, (size_t)(sleep_wake_end - sleep_wake_start));
	uint64_t top = arch_vm_la57() ? VIRT_TO_PHYS((uint64_t)pml5) : VIRT_TO_PHYS((uint64_t)pml4);
	*(volatile uint64_t *)(p + WAKE_DATA + WD_TOP) = top;
	*(volatile uint64_t *)(p + WAKE_DATA + WD_CTX) = (uint64_t)ctx;
	*(volatile uint64_t *)(p + WAKE_DATA + WD_ENTRY) = (uint64_t)sleep_resume_entry;
	__asm__ volatile("wbinvd" ::: "memory");
}

/* The APs saved before S3, one at a time through the one wake page. */
static void wake_aps_deep(void)
{
	for (int i = 1; i < smp_ncpus; i++) {
		if (park_state[i] != 1) continue;
		wake_page(&cpu_save[i]);
		arch_smp_wake_cpu(i, WAKE_PHYS);
		uint64_t t0 = mono_ns();
		while (park_state[i] != 2 && mono_ns() - t0 < 1000000000ULL)
			__asm__ volatile("pause" ::: "memory");
		if (park_state[i] != 2) {
			n_ap_lost++;
			kprintf(KERN_ERR "sleep: cpu%d did not come back from S3\n", i);
		}
	}
}

int arch_sleep_park_cpus(int kind)
{
	uint32_t a, b, c, d;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
	park_mwait = (c >> 3) & 1;
	park_kind = kind;
	park_hold = 1;
	smp_mb();
	int want = 0;
	for (int i = 1; i < smp_ncpus; i++) {
		if (!all_cpus[i] || !all_cpus[i]->online) continue;
		park_state[i] = 0;
		want++;
		arch_smp_park_send(i);
	}
	uint64_t t0 = mono_ns();
	for (;;) {
		int n = 0;
		for (int i = 1; i < smp_ncpus; i++) if (park_state[i] == 1) n++;
		if (n == want) return 0;
		if (mono_ns() - t0 > 1000000000ULL) break;
		__asm__ volatile("pause" ::: "memory");
	}
	n_park_fail++;
	kprintf(KERN_WARN "sleep: a CPU did not park within 1 s, not sleeping\n");
	arch_sleep_release_cpus(kind);
	return -EBUSY;
}

void arch_sleep_release_cpus(int kind)
{
	if (kind == SLEEP_DEEP) wake_aps_deep();
	smp_mb();
	park_hold = 0;
}

/* ---- s2idle ---- */

static uint64_t irq_count(uint32_t vec)
{
	uint64_t h;
	return irq_stat(vec, 0, &h);
}

static int s2idle(void)
{
	uint8_t m1 = inb(0x21), m2 = inb(0xA1);
	uint8_t w1 = (uint8_t)~((1u << 1) | (1u << 2)), w2 = (uint8_t)~(1u << 0);   /* keyboard, cascade, RTC */
	int sci = acpi_sci_irq();
	if (sci >= 0 && sci < 8) w1 &= (uint8_t)~(1u << sci);
	else if (sci >= 8) w2 &= (uint8_t)~(1u << (sci - 8));
	outb(0x21, w1);
	outb(0xA1, w2);
	acpi_sleep_wake_check(1);
	uint64_t keys = irq_count(0x21);
	int poll = sci < 0, w = WAKE_NONE;
	static volatile int never;
	sleep_frozen = 1;
	while (w == WAKE_NONE) {
		sleep_timer_fired = 0;
		lapic_sleep_arm(poll ? 1000000000ULL : 0);
		arch_idle_deep(&never, 1);              /* sti; mwait (or hlt): back after an interrupt */
		__asm__ volatile("cli");
		if ((w = acpi_sleep_wake_check(0))) break;
		if (irq_count(0x21) != keys) { w = WAKE_KEY; break; }
		if (rtc_af) {
			rtc_af = 0;
			if (alarm_due()) { w = WAKE_RTC; break; }
		}
		if (!sleep_timer_fired) n_spurious++;
	}
	sleep_frozen = 0;
	lapic_timer_mask();
	outb(0x21, m1);
	outb(0xA1, m2);
	n_s2idle++;
	return w;
}

/* ---- S3 ---- */

#define PM1_WAK_STS    0x8000u
#define PM1_RTC        0x0400u
#define PM1_PWRBTN     0x0100u
#define PM1_SCI_EN     0x0001u
#define PM1_SLP_EN     0x2000u

static uint16_t inw_(uint16_t p) { uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static void outw_(uint16_t p, uint16_t v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p)); }
static uint16_t pm1_en(void) { return (uint16_t)(SI.pm1a_evt + SI.pm1_len / 2); }

static int s3(void)
{
	uint8_t m1 = inb(0x21), m2 = inb(0xA1), e1 = inb(0x4D0), e2 = inb(0x4D1);
	uint16_t en_before = inw_(pm1_en());
	wake_page(&cpu_save[0]);
	*(volatile uint32_t *)(SI.facs + 12) = WAKE_PHYS;
	uint32_t flen;
	memcpy(&flen, SI.facs + 4, 4);
	if (flen >= 32) *(volatile uint64_t *)(SI.facs + 24) = 0;   /* the real mode vector, not X_ */

	outw_(SI.pm1a_evt, PM1_WAK_STS | PM1_RTC | PM1_PWRBTN);
	outw_(pm1_en(), (uint16_t)(en_before | PM1_PWRBTN | (alarm_at ? PM1_RTC : 0)));

	if (sleep_save_cpu(&cpu_save[0]) == 0) {
		__asm__ volatile("wbinvd" ::: "memory");
		uint16_t a = (uint16_t)((inw_(SI.pm1a_cnt) & ~0x3C00u) | ((unsigned)s3_a << 10));
		outw_(SI.pm1a_cnt, a);
		if (SI.pm1b_cnt) {
			uint16_t b = (uint16_t)((inw_(SI.pm1b_cnt) & ~0x3C00u) | ((unsigned)s3_b << 10));
			outw_(SI.pm1b_cnt, b);
			outw_(SI.pm1b_cnt, (uint16_t)(b | PM1_SLP_EN));
		}
		outw_(SI.pm1a_cnt, (uint16_t)(a | PM1_SLP_EN));
		for (int i = 0; i < 100000000; i++) __asm__ volatile("pause");
		kprintf(KERN_ERR "sleep: S3 entry did not take, the machine is still on\n");
		outw_(pm1_en(), en_before);
		return -EIO;
	}

	/* back from the firmware, on the kernel's stack, IRQs off */
	cpu_reinit(0);
	uart16550_init();
	pic8259_init();
	outb(0x4D0, e1);
	outb(0x4D1, e2);
	outb(0x21, m1);
	outb(0xA1, m2);
	uart16550_irq_init();
	hpet_resume();
	uint16_t sts = inw_(SI.pm1a_evt);
	outw_(SI.pm1a_evt, PM1_WAK_STS | PM1_RTC | PM1_PWRBTN);
	outw_(pm1_en(), en_before);
	acpi_ec_gpe_rearm();
	n_deep++;
	if (sts & PM1_PWRBTN) return WAKE_POWER;
	if (sts & PM1_RTC) return WAKE_RTC;
	return WAKE_OTHER;
}

int arch_sleep_enter(int kind, uint64_t *raw_before, uint64_t *raw_after, int64_t *rtc_before, int64_t *rtc_after)
{
	probe();
	if (kind == SLEEP_DEEP && !deep_ok) return -EINVAL;
	if ((int)arch_get_cpu_ctx()->cpu_id != 0) return -EBUSY;
	uint64_t f = irq_save();
	lapic_timer_mask();
	*rtc_before = arch_rtc_read();
	*raw_before = arch_clock_raw_ns();
	int w = kind == SLEEP_DEEP ? s3() : s2idle();
	*raw_after = arch_clock_raw_ns();
	*rtc_after = arch_rtc_read();
	if (w < 0) { lapic_timer_start(); irq_restore(f); return w; }
	(void)f;   /* IRQs stay off: suspend.c re-anchors the clock, then turns them on */
	return w;
}

/* After suspend.c re-anchored the clock: the BSP's tick again, IRQs on. */
void arch_sleep_after(void)
{
	lapic_timer_start();
	irq_enable();
}

static void sleep_diag(hw_out_t *o)
{
	probe();
	hw_begin(o, "power", "sleep");
	hw_kv(o, "deep_ok", (uint64_t)deep_ok);
	hw_kv(o, "s3_typ", s3_a);
	hw_kv(o, "s2idle", n_s2idle);
	hw_kv(o, "deep", n_deep);
	hw_kv(o, "spurious", n_spurious);
	hw_kv(o, "park_fail", n_park_fail);
	hw_kv(o, "ap_lost", n_ap_lost);
	hw_kv(o, "lps0", (uint64_t)((SI.fadt_flags >> 21) & 1));
	hw_end(o);
}

/* S3 cuts the power of every PCI device. None of Ranix's PCI drivers restores
 * its device yet, so each kind present blocks deep, by name; a driver that
 * learns to resume registers its own pm_ops instead and leaves this list. */
static const struct { uint8_t cls, sub, prog; const char *name; } powered_off[] = {
	{ 0x01, 0x06, 0x01, "ahci" }, { 0x01, 0x08, 0x02, "nvme" }, { 0x0C, 0x03, 0x30, "xhci" },
	{ 0x04, 0x03, 0x00, "hda" }, { 0x02, 0x00, 0x00, "ethernet" }, { 0x02, 0x80, 0x00, "wireless" },
	{ 0x03, 0x00, 0x00, "display" },
};
static pm_ops_t blockers[sizeof powered_off / sizeof powered_off[0] + 1];

void arch_sleep_init(void)
{
	/* one pass over the bus: each kind present once */
	uint32_t seen = 0;
	const unsigned nk = sizeof powered_off / sizeof powered_off[0];
	int n = 0;
	for (int bus = 0; bus < 256; bus++)
		for (int d = 0; d < 32; d++)
			for (int fn = 0; fn < 8; fn++) {
				uint32_t id = pci_read32((uint8_t)bus, (uint8_t)d, (uint8_t)fn, 0x00);
				if ((id & 0xFFFF) == 0xFFFF) { if (fn == 0) break; continue; }
				uint32_t c = pci_read32((uint8_t)bus, (uint8_t)d, (uint8_t)fn, 0x08);
				for (unsigned i = 0; i <= nk; i++) {
					int hit = i < nk ? (c >> 24) == powered_off[i].cls && ((c >> 16) & 0xFF) == powered_off[i].sub &&
					                   ((c >> 8) & 0xFF) == powered_off[i].prog
					                 : (id & 0xFFFF) == 0x1AF4;
					if (!hit || (seen & (1u << i))) continue;
					seen |= 1u << i;
					blockers[n].name = i < nk ? powered_off[i].name : "virtio";
					blockers[n].powers_off = 1;
					pm_register(&blockers[n++]);
				}
			}
	hwdiag_register(sleep_diag);
	/* now, at boot: the probe walks the boot loader's memory map, which a system
	 * call (the first read of /sys/power/mem_sleep) can no longer reach -- it
	 * faulted there on CR2 0x9b72d8 (SUSP1 KVM, 11-13) */
	probe();
}
