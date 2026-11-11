/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * ACPI events: the power button and, with the SCI on, the embedded controller's
 * GPE. Ranix runs no AML, so only what needs none is taken:
 *
 *   Power button  PWRBTN_STS in PM1 (fixed hardware). A press is a KEY_POWER key
 *                 (the desktop shows its shut-down question) and SIGPWR to init
 *                 (fjord-sys: with a desktop it lets the desktop ask; without
 *                 one, or pressed twice, it shuts down in order).
 *   EC GPE        the bit named by the EC device's Name (_GPE, n) in the DSDT,
 *                 a constant the AML need not run for. It wakes the EC event
 *                 reader (drivers/input/ec_keys.c) instead of a timer.
 *
 * Without ranix.sci=1 nothing here takes an interrupt: PM1 is read every 250 ms,
 * as before (PWRBTN_EN is set, the SCI line stays masked). With it:
 *   - the SCI line from the FADT is taken in level mode (ELCR), and the handler
 *     goes round until no enabled status bit is left;
 *   - PWRBTN_EN and the EC's GPE enable are set, nothing else. A status bit set
 *     and enabled but not ours is cleared and its enable turned off; counted;
 *   - the EC's GPE is off from the handler until the reader drained the EC, then
 *     on again without clearing its status, so a late event is not lost;
 *   - PM1 is still read once a second, a backup for a lost interrupt, and more
 *     than STORM_PER_S SCIs in a second mask the line and go back to polling.
 * Firmware in legacy mode (SCI_EN 0) keeps its events in SMM: ranix.sci=1 alone
 * then takes no SCI and says why. ranix.acpi_enable=1 writes ACPI_ENABLE, handing
 * heat and battery events to an OS that runs no AML for most of them: a test
 * flag, loud in the log, never a default.
 *
 * Doors: none. /proc/hwdiag "power acpi" shows the counters.
 */
#include <uapi/types.h>
#include <asm/cpu.h>
#include <asm/io.h>
#include <kernel/cmdline.h>
#include <kernel/hwdiag.h>
#include <kernel/input.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>
#include <kernel/proc.h>
#include <kernel/time.h>
#include <kernel/board.h>
#include <drivers/irqchip/irqchip.h>
#include <drivers/irqchip/pic8259/pic8259.h>
#include <drivers/platform/casper_ec.h>

#define PM1_PWRBTN  0x0100u
#define PM1_SCI_EN  0x0001u

#define SCI_ROUNDS   8      /* the handler goes round until nothing enabled is set, at most this often */
#define STORM_PER_S  200    /* more SCIs than this in a second: back to polling */

static acpi_ev_info_t A;
static volatile int sci_on;
static int ec_gpe = -1, elcr_level, handed_over;
static volatile int btn_pending;
static volatile uint32_t scis;
static uint32_t presses, foreign, ec_gpes, storms, backup_presses;

static uint16_t inw_p(uint16_t p) { uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static void outw_p(uint16_t p, uint16_t v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p)); }
static uint16_t pm1_en_port(void) { return (uint16_t)(A.pm1a_evt + A.pm1_len / 2); }
static uint16_t gpe_sts(int byte) { return (uint16_t)(A.gpe0_blk + byte); }
static uint16_t gpe_en(int byte)  { return (uint16_t)(A.gpe0_blk + A.gpe0_len / 2 + byte); }

/* Name (_GPE, n) as AML bytes: NameOp '_GPE' BytePrefix n. One value, or -1. */
static int find_ec_gpe(const uint8_t *d, uint32_t len)
{
	int v = -1;
	for (uint32_t i = 36; i + 7 <= len; i++) {
		if (d[i] != 0x08 || memcmp(d + i + 1, "_GPE", 4) || d[i + 5] != 0x0A) continue;
		if (v >= 0 && v != d[i + 6]) return -1;          /* two ECs, two answers: take none */
		v = d[i + 6];
	}
	return v;
}

static void power_pressed(void)
{
	presses++;
	kprintf(KERN_INFO "acpi: power button\n");
	input_report_key(KEY_POWER, 1);
	input_sync();
	input_report_key(KEY_POWER, 0);
	input_sync();
	process_t *init = proc_get(1);
	if (init) {
		task_signal(init, 30);     /* SIGPWR */
		proc_put(init);
	}
}

/* ---- the SCI: interrupt context ----
 * One round reads PM1 and GPE0 and answers every enabled status bit that is set.
 * A level line drops only when none is left, and an edge line gets no new edge
 * while one is left, so the handler goes round until a round finds nothing. */
static int sci_round(void)
{
	int any = 0;
	uint16_t sts = inw_p(A.pm1a_evt), en = inw_p(pm1_en_port());
	if (sts & PM1_PWRBTN) { outw_p(A.pm1a_evt, PM1_PWRBTN); btn_pending = 1; any = 1; }
	uint16_t other = (uint16_t)(sts & en & ~PM1_PWRBTN);
	if (other) {                                  /* enabled, set, and nobody here answers it */
		outw_p(A.pm1a_evt, other);
		outw_p(pm1_en_port(), (uint16_t)(en & ~other));
		foreign++;
		any = 1;
	}
	for (int b = 0; b < A.gpe0_len / 2; b++) {
		uint8_t s = inb(gpe_sts(b)), e = inb(gpe_en(b));
		uint8_t hit = s & e;
		if (!hit) continue;
		any = 1;
		uint8_t mine = (ec_gpe >= 0 && ec_gpe / 8 == b) ? (uint8_t)(1u << (ec_gpe % 8)) : 0;
		if (hit & mine) {
			outb(gpe_en(b), (uint8_t)(e & ~mine));  /* off until the reader drained the EC */
			outb(gpe_sts(b), mine);
			ec_gpes++;
			ec_events_kick();
		}
		if (hit & ~mine) {
			outb(gpe_en(b), (uint8_t)(inb(gpe_en(b)) & ~(hit & ~mine)));
			outb(gpe_sts(b), (uint8_t)(hit & ~mine));
			foreign++;
		}
	}
	return any;
}

static void sci_handler(void)
{
	scis++;
	if (!sci_on) return;                          /* fell back to polling: the line is masked */
	for (int i = 0; i < SCI_ROUNDS && sci_round(); i++) { }
}

/* The EC reader drained the queue: its GPE may fire again. */
void acpi_ec_gpe_rearm(void)
{
	if (!sci_on || ec_gpe < 0) return;
	int b = ec_gpe / 8;
	uint8_t bit = (uint8_t)(1u << (ec_gpe % 8));
	uint64_t f = irq_save();
	/* the status bit is not cleared here: an event that came after the reader's
	 * last empty query must still raise the SCI the moment the enable is back */
	outb(gpe_en(b), (uint8_t)(inb(gpe_en(b)) | bit));
	irq_restore(f);
}

/* The SCI line in level mode, as the FADT's SCI is shared, active low and level
 * (ACPI 5.2.9): the 8259's ELCR, ports 0x4D0/0x4D1, one bit per line. */
static void elcr_set_level(unsigned irq)
{
	uint16_t port = irq < 8 ? 0x4D0 : 0x4D1;
	uint8_t bit = (uint8_t)(1u << (irq & 7));
	outb(port, (uint8_t)(inb(port) | bit));
	elcr_level = (inb(port) & bit) != 0;
}

static int sci_start(void)
{
	if (A.sci_int == 0 || A.sci_int >= 16 || A.sci_int == 2) return -1;
	char v[4];
	if (!(inw_p(A.pm1a_cnt) & PM1_SCI_EN)) {
		/* Legacy mode: the firmware's SMM handles the events itself (heat, battery,
		 * the EC's queries). Writing ACPI_ENABLE hands them to the OS, and Ranix runs
		 * no AML for most of them. Only with its own flag, and said aloud. */
		if (!(cmdline_get("ranix.acpi_enable", v, sizeof(v)) && !strcmp(v, "1")) || !A.smi_cmd || !A.acpi_enable) {
			kprintf(KERN_WARN "acpi: firmware in legacy mode (SCI_EN 0): no SCI, the button is polled; "
			        "ranix.acpi_enable=1 hands the events to the OS\n");
			return -1;
		}
		kprintf(KERN_WARN "acpi: handing the firmware's events to the OS (ACPI_ENABLE 0x%x to port 0x%x)\n",
		        A.acpi_enable, A.smi_cmd);
		outb((uint16_t)A.smi_cmd, A.acpi_enable);
		for (int i = 0; i < 300 && !(inw_p(A.pm1a_cnt) & PM1_SCI_EN); i++) task_sleep_until(kernel_ticks + 2);
		handed_over = 1;
	}
	if (!(inw_p(A.pm1a_cnt) & PM1_SCI_EN)) { kprintf(KERN_WARN "acpi: SCI_EN stays off, no SCI\n"); return -1; }
	irq_register(0x20 + A.sci_int, sci_handler);
	elcr_set_level(A.sci_int);
	outw_p(A.pm1a_evt, PM1_PWRBTN);
	outw_p(pm1_en_port(), (uint16_t)(inw_p(pm1_en_port()) | PM1_PWRBTN));
	if (A.gpe0_blk && A.dsdt) ec_gpe = find_ec_gpe(A.dsdt, A.dsdt_len);
	if (ec_gpe >= A.gpe0_len / 2 * 8) ec_gpe = -1;
	if (ec_gpe >= 0 && casper_ec_board()) ec_events_use_sci();
	else ec_gpe = -1;                             /* a GPE nobody here reads is never enabled */
	sci_on = 1;
	acpi_ec_gpe_rearm();
	irqchip_enable_irq(0x20 + A.sci_int);
	kprintf(KERN_INFO "acpi: SCI on irq %u (%s), power button%s\n", A.sci_int, elcr_level ? "level" : "edge",
	        ec_gpe >= 0 ? ", EC GPE" : "");
	return 0;
}

/* Back to polling: a line that storms is masked and never taken again this boot. */
static void sci_give_up(uint32_t n)
{
	sci_on = 0;
	pic8259_mask(A.sci_int);
	storms++;
	kprintf(KERN_WARN "acpi: %u SCIs in a second, back to polling the button\n", n);
}

static void acpi_button_thread(void)
{
	uint64_t mark = kernel_ticks, polled = kernel_ticks;
	uint32_t at_mark = scis;
	for (;;) {
		task_sleep_until(kernel_ticks + (sci_on ? TIMER_HZ / 10 : TIMER_HZ / 4));
		if (sci_on && kernel_ticks - mark >= TIMER_HZ) {      /* the storm guard */
			uint32_t n = scis - at_mark;
			if (n > STORM_PER_S) sci_give_up(n);
			mark = kernel_ticks;
			at_mark = scis;
		}
		int pressed = 0;
		if (sci_on && btn_pending) { btn_pending = 0; pressed = 1; }
		/* polled: every 250 ms; with the SCI, once a second as a backup for a lost edge */
		if (!pressed && (!sci_on || kernel_ticks - polled >= TIMER_HZ)) {
			polled = kernel_ticks;
			if (inw_p(A.pm1a_evt) & PM1_PWRBTN) {
				outw_p(A.pm1a_evt, PM1_PWRBTN);
				pressed = 1;
				if (sci_on) backup_presses++;
			}
		}
		if (pressed) power_pressed();
	}
}

static void acpi_diag(hw_out_t *o)
{
	hw_begin(o, "power", "acpi");
	hw_kv(o, "sci_en", inw_p(A.pm1a_cnt) & 1);
	hw_kv(o, "sci", (uint64_t)sci_on);
	hw_kv(o, "sci_irq", A.sci_int);
	hw_kv(o, "scis", scis);
	hw_kv(o, "presses", presses);
	hw_kv(o, "ec_gpe", (uint64_t)(ec_gpe + 1));      /* 0: none */
	hw_kv(o, "ec_gpes", ec_gpes);
	hw_kv(o, "foreign", foreign);
	hw_kv(o, "elcr_level", (uint64_t)elcr_level);
	hw_kv(o, "handed_over", (uint64_t)handed_over);
	hw_kv(o, "storms", storms);
	hw_kv(o, "backup_presses", backup_presses);
	hw_kv(o, "pm1_sts", inw_p(A.pm1a_evt));
	hw_kv(o, "pm1_en", inw_p(pm1_en_port()));
	hw_end(o);
}

void acpi_button_start(void)
{
	char v[4];
	if (acpi_events_info(&A)) return;
	outw_p(A.pm1a_evt, PM1_PWRBTN);       /* a press from before does not count */
	/* PWRBTN_EN as Linux sets it: some chipsets (QEMU's PIIX4) set the status bit
	 * only when it is enabled. Polled, the SCI line stays masked at the 8259. */
	outw_p(pm1_en_port(), (uint16_t)(inw_p(pm1_en_port()) | PM1_PWRBTN));
	hwdiag_register(acpi_diag);
	if (cmdline_get("ranix.sci", v, sizeof(v)) && !strcmp(v, "1")) sci_start();
	proc_create("acpi_button", acpi_button_thread);
}
