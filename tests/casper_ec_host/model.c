/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* EC mailbox model for drivers/platform/casper_ec.c. QEMU has no such EC, so this
 * is the only way the driver's mailbox sequence runs before the real laptop.
 *
 * The model follows the board's AML (EC0.RRAM/WRAM, DSDT OEM CASPER/ERA): writing
 * 0x0F to ESEM rings the EC; after a configurable number of 1 ms sleeps it runs
 * CMD1 (0x80 read, 0x81 write) on EC RAM at CDT1:CDT2, puts a read's byte in
 * EDA1, clears CMD1 and ESEM and scribbles CDT1..CDT3, so that every byte the
 * driver writes for the next command is a visible change. Every mailbox access
 * must sit inside its own global-lock hold: one changed byte at most per hold.
 *
 * Proves the byte order, the poll count, the bit arithmetic of on/off, the
 * failure paths and the board match. It cannot prove what the real EC does with
 * the bytes.
 */
#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "stub/model.h"
#include "../../drivers/platform/casper_ec.h"
#include "../../drivers/platform/casper_power.h"
#include <kernel/power_supply.h>

#define MB_PHYS 0xFEFF0700ULL
#define SCRIBBLE 0xEE

volatile uint64_t kernel_ticks;

static struct {
	uint32_t subsys, vid;
	const char *oem, *table, *cmdline;
	int ec_delay;              /* sleeps before the EC answers; <0 never */
	int fail_write_n;          /* the n-th write command (1-based) never answers */
	int lock_busy;
	uint8_t mb[0x80], shadow[0x80];
	uint8_t ram[0x800];
	uint8_t ram1[0x80];        /* ECW1, the shared RAM at 0xFEFF0380 */
	uint8_t pin[256];          /* RPIN answers, by CDT2 */
	int ram1_maps;
	uint8_t evq[8]; int nev, obf, evdata, port_cmds, port_ins;   /* the 0x62/0x66 event ports */
	int held, holds, multi;    /* lock depth, holds, holds that changed >1 byte */
	int busy, left, cmd_writes;
	int sleeps, ioremaps;
	uint64_t ioremap_phys;
	uint32_t ioremap_flags;
	int log[512][2], nlog;     /* every mailbox byte written: offset, value */
	void (*thread)(void);
	void (*power_thread)(void);
	ps_t ps[4]; int nps;       /* /sys/class/power_supply as casper_power.c leaves it */
	int lid;
	char klog[8192];
} m;
static jmp_buf parked;

static void reset_model(void)
{
	memset(&m, 0, sizeof(m));
	m.subsys = 0x22c41b0a; m.vid = 0x59168086;
	m.oem = "CASPER"; m.table = "ERA     ";
	memset(m.mb, SCRIBBLE, sizeof(m.mb));
	m.mb[0] = 0; m.mb[1] = 0;
	memcpy(m.shadow, m.mb, sizeof(m.mb));
	m.ram[0x0616] = 0x05; m.ram[0x04A0] = 0x21; m.ram[0x0617] = 0x00;
	kernel_ticks = 1000;
}

static void ec_run(void)
{
	uint16_t a = (uint16_t)(m.mb[2] << 8 | m.mb[3]);
	assert(a < sizeof(m.ram));
	if (m.mb[1] == 0x80) m.mb[0x40] = m.ram[a];
	else if (m.mb[1] == 0x81) m.ram[a] = m.mb[4];
	else if (m.mb[1] == 0x87) m.mb[0x40] = m.pin[a & 0xFF];
	else assert(!"unknown EC command");
	m.mb[1] = 0; m.mb[0] = 0;
	m.mb[2] = m.mb[3] = m.mb[4] = SCRIBBLE;
	memcpy(m.shadow, m.mb, sizeof(m.mb));
	m.busy = 0;
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
	if (bus || dev != 2 || fn) return 0xFFFFFFFF;
	return reg == 0 ? m.vid : reg == 0x2C ? m.subsys : 0xFFFFFFFF;
}

void *ioremap(uint64_t phys, size_t size, uint32_t flags)
{
	assert(size == 0x80);
	if (phys == 0xFEFF0380ULL) { m.ram1_maps++; assert(!(flags & VM_WRITE)); return m.ram1; }   /* read only */
	m.ioremaps++; m.ioremap_phys = phys; m.ioremap_flags = flags;
	return m.mb;
}

void kprintf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	size_t n = strlen(m.klog);
	vsnprintf(m.klog + n, sizeof(m.klog) - n, fmt, ap);
	va_end(ap);
}

int mutex_lock_killable(mutex_t *mx) { assert(!mx->locked); mx->locked = 1; return 0; }
void mutex_unlock(mutex_t *mx) { assert(mx->locked); mx->locked = 0; }

void task_sleep_until(uint64_t deadline)
{
	assert(!m.held);                               /* never sleep holding the global lock */
	if (deadline > kernel_ticks + 1000000) longjmp(parked, 1);
	assert(deadline >= kernel_ticks + 2);          /* at least a full millisecond */
	kernel_ticks = deadline;
	m.sleeps++;
	if (m.busy && m.left > 0 && --m.left == 0) ec_run();
}

pid_t proc_create(const char *name, void (*fn)(void))
{
	if (!strcmp(name, "casper_power")) { m.power_thread = fn; return 8; }
	assert(!strcmp(name, "casper_ec"));
	m.thread = fn;
	return 7;
}

/* kernel/power_supply.c, as a list: add or update by name, remove by name */
void power_supply_set(const ps_t *p)
{
	int i = 0;
	while (i < m.nps && strcmp(m.ps[i].name, p->name)) i++;
	assert(i < 4);
	m.ps[i] = *p;
	if (i == m.nps) m.nps++;
}

void power_supply_remove(const char *name)
{
	for (int i = 0; i < m.nps; i++)
		if (!strcmp(m.ps[i].name, name)) {
			for (int j = i + 1; j < m.nps; j++) m.ps[j - 1] = m.ps[j];
			m.nps--;
			return;
		}
}

void power_lid_set(int open) { m.lid = open; }
void hwdiag_register(hwdiag_fn fn) { (void)fn; }
void hw_begin(hw_out_t *o, const char *cls, const char *name) { (void)o; (void)cls; (void)name; }
void hw_kv(hw_out_t *o, const char *key, uint64_t v) { (void)o; (void)key; (void)v; }
void hw_end(hw_out_t *o) { (void)o; }
void input_report_switch(uint16_t code, int32_t value) { (void)code; (void)value; }
void backlight_unblank(void) {}

const char *cmdline_get(const char *key, char *buf, size_t bufsz)
{
	size_t k = strlen(key);
	if (!m.cmdline || strncmp(m.cmdline, key, k) || m.cmdline[k] != '=') return NULL;
	snprintf(buf, bufsz, "%s", m.cmdline + k + 1);
	return buf;
}

int arch_fw_lock(uint32_t timeout_ms)
{
	assert(timeout_ms > 0 && !m.held);
	if (m.lock_busy) return -16;
	m.held = 1;
	return 0;
}

void arch_fw_unlock(void)
{
	assert(m.held);
	m.held = 0;
	m.holds++;
	int changed = 0;
	for (int i = 0; i < 0x80; i++) {
		if (m.mb[i] == m.shadow[i]) continue;
		changed++;
		assert(m.nlog < 512);
		m.log[m.nlog][0] = i; m.log[m.nlog][1] = m.mb[i]; m.nlog++;
	}
	if (changed > 1) m.multi++;
	memcpy(m.shadow, m.mb, sizeof(m.mb));
	if (changed && m.mb[0] == 0x0F && m.log[m.nlog - 1][0] == 0) {   /* the doorbell */
		if (m.busy && m.left < 0) return;              /* a dead EC stays dead */
		assert(!m.busy);
		m.busy = 1;
		if (m.mb[1] == 0x81 && ++m.cmd_writes == m.fail_write_n) { m.left = -1; return; }
		m.left = m.ec_delay;
		if (m.left == 0) ec_run();
	}
}

uint8_t inb(uint16_t port)
{
	m.port_ins++;
	if (port == 0x80) return 0;
	if (port == 0x66) return (uint8_t)((m.nev ? 0x20 : 0) | (m.obf ? 0x01 : 0));
	assert(port == 0x62);
	m.obf = 0;
	return (uint8_t)m.evdata;
}

void outb(uint16_t port, uint8_t v)
{
	assert(port == 0x66 && v == 0x84 && !m.held);   /* only QR_EC, never inside the mailbox lock */
	m.port_cmds++;
	m.evdata = m.nev ? m.evq[0] : 0;
	for (int i = 1; i < m.nev; i++) m.evq[i - 1] = m.evq[i];
	if (m.nev) m.nev--;
	m.obf = 1;
}

int arch_fw_oem(char oem[7], char table[9])
{
	snprintf(oem, 7, "%s", m.oem);
	snprintf(table, 9, "%s", m.table);
	return 0;
}

/* ── helpers ── */

static int cmd(const char *s) { return casper_ec_command(s, strlen(s)); }

static const char *status(void)
{
	static char b[512];
	int n = casper_ec_status(b, sizeof(b));
	assert(n > 0 && (size_t)n < sizeof(b) && b[n - 1] == '\n');
	return b;
}

/* the bytes one RRAM (val < 0) or WRAM writes, from log position *at */
static void expect_cmd(int *at, uint16_t addr, int val)
{
	int want[5][2] = { { 3, addr & 0xFF }, { 2, addr >> 8 }, { 4, val }, { 1, val < 0 ? 0x80 : 0x81 }, { 0, 0x0F } };
	for (int i = 0; i < 5; i++) {
		if (i == 2 && val < 0) continue;
		if (m.log[*at][0] != want[i][0] || m.log[*at][1] != want[i][1]) {
			fprintf(stderr, "log %d: got [%d]=0x%x, want [%d]=0x%x (addr 0x%x)\n", *at,
			        m.log[*at][0], m.log[*at][1], want[i][0], want[i][1], addr);
			abort();
		}
		(*at)++;
	}
}

/* ── scenarios, each in its own process so the driver's state starts clean ── */

static void t_other_board(void)
{
	m.subsys = 0x12341b0a;
	casper_ec_init();
	assert(m.ioremaps == 0 && !m.thread);
	assert(cmd("on") == -19 && cmd("read") == -19);    /* ENODEV */
	assert(m.holds == 0 && m.nlog == 0);
	assert(strstr(status(), "board none (GPU subsystem)"));
}

static void t_other_dsdt(void)
{
	m.oem = "CASPEX";
	m.cmdline = "ranix.ec_osflags=1";
	casper_ec_init();
	assert(m.ioremaps == 0 && !m.thread);
	assert(cmd("on") == -19 && m.holds == 0);
	assert(strstr(status(), "board none (DSDT OEM)"));
}

static void t_other_table(void)
{
	m.table = "ERB     ";
	casper_ec_init();
	assert(m.ioremaps == 0 && cmd("on") == -19);
}

static void t_not_intel(void)
{
	m.vid = 0x12341002;
	casper_ec_init();
	assert(m.ioremaps == 0 && cmd("on") == -19);
}

static void t_bad_command(void)
{
	casper_ec_init();
	assert(cmd("bogus") == -22 && cmd("o") == -22 && cmd("onn") == -22 && cmd("") == -22);
	assert(m.holds == 0);
}

static void t_default_off(void)
{
	casper_ec_init();
	assert(m.ioremaps == 1 && m.ioremap_phys == MB_PHYS);
	assert(m.ioremap_flags == (VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE));
	assert(!m.thread && m.holds == 0 && m.nlog == 0);
	assert(strstr(status(), "board casper\nosflags off\n"));
	assert(strstr(status(), "before - - -\n"));
}

static void t_on_off(void)
{
	m.ec_delay = 3;
	casper_ec_init();
	assert(cmd("on\n") == 0);
	assert(m.multi == 0);
	int at = 0;
	expect_cmd(&at, 0x0616, -1); expect_cmd(&at, 0x04A0, -1); expect_cmd(&at, 0x0617, -1);
	expect_cmd(&at, 0x0616, 0x04); expect_cmd(&at, 0x04A0, 0x29); expect_cmd(&at, 0x0617, 0x01);
	expect_cmd(&at, 0x0616, -1); expect_cmd(&at, 0x04A0, -1); expect_cmd(&at, 0x0617, -1);
	assert(at == m.nlog);
	/* per read: 4 writes + 4 CMD1 polls + final CMD1 + EDA1; per write: 5 + 4 + 1 */
	assert(m.holds == 6 * 10 + 3 * 10);
	assert(m.sleeps == 9 * 3);
	assert(m.ram[0x0616] == 0x04 && m.ram[0x04A0] == 0x29 && m.ram[0x0617] == 0x01);
	assert(strstr(status(), "osflags on\nbytes 0x616 0x4a0 0x617\nbefore 0x5 0x21 0x0\nnow 0x4 0x29 0x1\n"));
	assert(strstr(m.klog, "casper_ec: before 0x616=0x5 0x4a0=0x21 0x617=0x0"));
	assert(strstr(m.klog, "casper_ec: after 0x616=0x4 0x4a0=0x29 0x617=0x1"));

	int n = m.nlog;
	assert(cmd("on") == 0 && m.nlog == n);              /* already on: nothing */

	m.ram[0x04A0] |= 0x02;                              /* the EC changes its own bits meanwhile */
	m.ram[0x0616] |= 0x80;
	assert(cmd("off") == 0);
	at = n;
	expect_cmd(&at, 0x0617, -1); expect_cmd(&at, 0x0617, 0x00);
	expect_cmd(&at, 0x04A0, -1); expect_cmd(&at, 0x04A0, 0x23);
	expect_cmd(&at, 0x0616, -1); expect_cmd(&at, 0x0616, 0x85);
	assert(m.ram[0x0616] == 0x85 && m.ram[0x04A0] == 0x23 && m.ram[0x0617] == 0x00);
	assert(strstr(status(), "osflags off\n"));
	n = m.nlog;
	assert(cmd("off") == 0 && m.nlog == n);             /* already off: nothing */
	assert(cmd("read") == 0 && strstr(status(), "now 0x85 0x23 0x0\n"));
}

static void t_already_set(void)
{
	m.ram[0x0616] = 0x04; m.ram[0x04A0] = 0x29; m.ram[0x0617] = 0x01;   /* as a warm boot from Linux leaves it */
	casper_ec_init();
	assert(cmd("on") == 0 && cmd("off") == 0);
	assert(m.ram[0x0616] == 0x04 && m.ram[0x04A0] == 0x29 && m.ram[0x0617] == 0x01);
}

static void t_ec_silent(void)
{
	m.ec_delay = -1;
	casper_ec_init();
	uint64_t t0 = kernel_ticks;
	assert(cmd("on") == -110);                          /* ETIMEDOUT on the first read */
	assert(m.sleeps == 127 && kernel_ticks - t0 >= 127);
	assert(m.nlog == 4 && m.ram[0x04A0] == 0x21);       /* one RRAM, no write */
	assert(strstr(status(), "osflags off\n") && strstr(status(), "timeouts 1 ") && strstr(status(), "last_error -110"));
	assert(strstr(m.klog, "nothing written"));
}

static void t_lock_busy(void)
{
	m.lock_busy = 1;
	casper_ec_init();
	assert(cmd("on") == -16 && m.nlog == 0);
	assert(strstr(status(), "osflags off\n") && strstr(status(), "lock_busy 1 "));
}

static void t_write_fails(void)
{
	m.ec_delay = 1;
	m.fail_write_n = 2;                                  /* the 0x04A0 write never answers */
	casper_ec_init();
	assert(cmd("on") == -110);
	assert(m.ram[0x0616] == 0x04 && m.ram[0x04A0] == 0x21 && m.ram[0x0617] == 0x00);
	assert(strstr(status(), "osflags on\n"));            /* half done: "off" must still undo it */
	m.busy = 0; m.mb[1] = 0; m.mb[0] = 0; memcpy(m.shadow, m.mb, sizeof(m.mb));   /* the EC recovers */
	assert(cmd("off") == 0);
	assert(m.ram[0x0616] == 0x05 && m.ram[0x04A0] == 0x21 && m.ram[0x0617] == 0x00);
}

static void t_boot_flag(void)
{
	m.cmdline = "ranix.ec_osflags=1";
	casper_ec_init();
	assert(m.thread && m.nlog == 0);                     /* boot does not wait for the EC */
	if (!setjmp(parked)) { m.thread(); assert(!"thread returned"); }
	assert(m.ram[0x04A0] == 0x29 && strstr(status(), "osflags on\n"));
}

static void t_boot_flag_zero(void)
{
	m.cmdline = "ranix.ec_osflags=0";
	casper_ec_init();
	assert(!m.thread);
}

/* ECW1: the bytes come across in one lock hold, nothing is written anywhere */
static void t_ram1(void)
{
	casper_ec_init();
	assert(m.ram1_maps == 1);
	for (int i = 0; i < 0x80; i++) m.ram1[i] = (uint8_t)(i * 7);
	uint8_t b[0x60];
	int h = m.holds;
	assert(casper_ec_ram1(b, 0x60) == 0);
	for (int i = 0; i < 0x60; i++) assert(b[i] == (uint8_t)(i * 7));
	assert(m.holds == h + 1 && m.nlog == 0);
	m.lock_busy = 1;
	assert(casper_ec_ram1(b, 0x60) == -16);       /* EBUSY: firmware holds the lock */
	assert(casper_ec_ram1(b, 0x81) == -19);
	assert(strstr(status(), "ram_reads 1 "));
}

/* the event query goes through the same driver: one QR_EC per event */
static void t_event(void)
{
	casper_ec_init();
	assert(casper_ec_event() == -1 && m.port_cmds == 0);   /* EC_NONE: nothing waits, nothing asked */
	m.evq[0] = 0xA0; m.evq[1] = 0x81; m.nev = 2;
	assert(casper_ec_event() == 0xA0);
	assert(casper_ec_event() == 0x81);
	assert(casper_ec_event() == -1 && m.port_cmds == 2);
	assert(strstr(status(), "queries 4"));
}

/* RPIN (4, 6), the lid: CDT2 = 4 << 4 | 6, CDT1 = 0, CMD1 = 0x87, ring; EDA1 */
static void t_pin(void)
{
	casper_ec_init();
	m.pin[0x46] = 0;
	assert(casper_ec_pin(4, 6) == 0);
	int want[4][2] = { { 3, 0x46 }, { 2, 0x00 }, { 1, 0x87 }, { 0, 0x0F } };
	for (int i = 0; i < 4; i++) assert(m.log[i][0] == want[i][0] && m.log[i][1] == want[i][1]);
	m.pin[0x46] = 1;
	assert(casper_ec_pin(4, 6) == 1);
	m.pin[0x46] = 0xFF;
	assert(casper_ec_pin(4, 6) == 1);                /* AML: Ones reads as open */
	m.pin[0x47] = 1;
	assert(casper_ec_pin(0x45, 7) == 1);              /* port 0x45 is 'E': folds to 4, as RPIN does */
	assert(m.multi == 0);
}

/* a late answer left in the output buffer: the third query drops it, events flow again */
static void t_stale(void)
{
	casper_ec_init();
	m.obf = 1; m.evdata = 0x55;
	assert(casper_ec_event() == -2 && casper_ec_event() == -2);   /* EC_BUSY */
	assert(casper_ec_event() == -2 && !m.obf);                    /* dropped, said busy */
	assert(strstr(status(), "stale 1"));
	m.evq[0] = 0xA0; m.nev = 1;
	assert(casper_ec_pending() == 1);
	assert(casper_ec_event() == 0xA0);
	assert(casper_ec_pending() == 0);
}

static void t_other_board_paths(void)
{
	m.subsys = 0x12341b0a;
	casper_ec_init();
	uint8_t b[4];
	assert(casper_ec_ram1(b, 4) == -19 && casper_ec_event() == -1 && casper_ec_pin(4, 6) == -19);
	assert(m.port_cmds == 0 && m.port_ins == 0 && m.ram1_maps == 0);
}


/* ── casper_power.c on ECW1: what /sys/class/power_supply lists ── */

static const char *supplies(void)
{
	static char b[64];
	b[0] = 0;
	for (int i = 0; i < m.nps; i++) { if (i) strcat(b, " "); strcat(b, m.ps[i].name); }
	return b;
}

static const ps_t *supply(const char *name)
{
	for (int i = 0; i < m.nps; i++) if (!strcmp(m.ps[i].name, name)) return &m.ps[i];
	return NULL;
}

static void put16(int off, unsigned v) { m.ram1[off] = (uint8_t)v; m.ram1[off + 1] = (uint8_t)(v >> 8); }

/* ECW1 as the EC keeps it. bat 0: bay empty (EPWS bit1 clear), as on a Casper Nirvana
 * with the battery out. design/full: 0, 0xFFFF (the AML's Ones) or a capacity. */
static void ecw1(int ac, int bat, unsigned rc, unsigned full, unsigned design)
{
	memset(m.ram1, 0, sizeof(m.ram1));
	m.ram1[0x01] = (uint8_t)((ac ? 1 : 0) | (bat ? 2 : 0));
	m.ram1[0x02] = 0x01;
	m.ram1[0x06] = bat ? 0x01 : 0xFF;
	put16(0x12, bat ? 11400 : 0xFFFF);
	put16(0x14, rc); put16(0x16, full); put16(0x18, 0); put16(0x1A, 0);
	put16(0x50, design); put16(0x52, bat ? 11400 : 0xFFFF); put16(0x56, 0x0000);
}

static void power_up(void)
{
	casper_ec_init();
	casper_power_init();
	assert(m.power_thread);
}

static void poll(void) { casper_power_event(2); }        /* ECP_BATTERY: one refresh, as the thread does */

static void no_fake_battery(void)
{
	assert(!strcmp(supplies(), "AC"));
	assert(!supply("BAT0"));
	assert(!strstr(m.klog, "-1%") && !strstr(m.klog, "battery unknown"));
}

/* a Casper Nirvana run on the adapter only: the bay empty, design/full 0 */
static void t_bat_absent(void)
{
	ecw1(1, 0, 0, 0, 0);
	power_up();
	m.klog[0] = 0;                                       /* from here on: what the reads say */
	poll();
	assert(supply("AC") && supply("AC")->online == 1);
	assert(strstr(m.klog, "casper_power: adapter in\n") && !strstr(m.klog, "battery"));   /* hidden, silently */
	no_fake_battery();
	poll(); poll();
	no_fake_battery();
}

/* the same with every capacity field unknown (0xFFFF, the AML's Ones) */
static void t_bat_absent_unknown(void)
{
	ecw1(1, 0, 0xFFFF, 0xFFFF, 0xFFFF);
	power_up();
	poll();
	no_fake_battery();
}

/* no battery, then the EC RAM is unreadable three times: still no BAT0 */
static void t_bat_absent_ec_fails(void)
{
	ecw1(1, 0, 0, 0, 0);
	power_up();
	poll();
	m.lock_busy = 1;
	poll(); poll(); poll(); poll();
	no_fake_battery();
	m.lock_busy = 0;
	poll();
	no_fake_battery();
}

/* never read at all, then three failures: nothing is invented */
static void t_bat_never_read(void)
{
	ecw1(1, 0, 0, 0, 0);
	m.lock_busy = 1;
	power_up();
	poll(); poll(); poll();
	assert(m.nps == 0);
}

/* a battery in its bay: listed, with its percentage */
static void t_bat_present(void)
{
	ecw1(0, 1, 2050, 4000, 4100);
	power_up();
	poll();
	assert(!strcmp(supplies(), "AC BAT0"));
	const ps_t *b = supply("BAT0");
	assert(b->present == 1 && b->capacity == 51 && b->status == PS_ST_DISCHARGING);
	assert(strstr(m.klog, "battery 51%"));
}

/* a battery whose full capacity is 0 or unknown: listed, but no percentage at all */
static void t_bat_present_no_full(void)
{
	ecw1(1, 1, 2000, 0, 0);
	power_up();
	poll();
	assert(supply("BAT0") && supply("BAT0")->capacity == -1);
	ecw1(1, 1, 2000, 0xFFFF, 0xFFFF);
	poll();
	assert(supply("BAT0") && supply("BAT0")->capacity == -1);
	ecw1(1, 1, 0xFFFF, 4000, 4100);
	poll();
	assert(supply("BAT0") && supply("BAT0")->capacity == -1);
	assert(!strstr(m.klog, "-1%"));
}

/* unplugged (present 1 -> 0): BAT0 leaves the list, as on Linux; back in: listed again */
static void t_bat_unplug(void)
{
	ecw1(1, 1, 3000, 4000, 4100);
	power_up();
	poll();
	assert(!strcmp(supplies(), "AC BAT0"));
	ecw1(1, 0, 0, 0, 0);
	poll();
	assert(!strcmp(supplies(), "AC"));
	ecw1(1, 1, 3000, 4000, 4100);
	poll();
	assert(!strcmp(supplies(), "AC BAT0") && supply("BAT0")->capacity == 75);
}

/* a battery seen, then the EC RAM unreadable: "Unknown" rather than stale data (kept) */
static void t_bat_seen_then_fails(void)
{
	ecw1(1, 1, 3000, 4000, 4100);
	power_up();
	poll();
	m.lock_busy = 1;
	poll(); poll();
	assert(supply("BAT0")->capacity == 75);
	poll();
	const ps_t *b = supply("BAT0");
	assert(b && b->present == 1 && b->status == PS_ST_UNKNOWN && b->capacity == -1);
}

static const struct { const char *name; void (*fn)(void); } T[] = {
	{ "ram1", t_ram1 }, { "event", t_event }, { "stale", t_stale }, { "pin", t_pin }, { "other-board-paths", t_other_board_paths },
	{ "other-board", t_other_board }, { "other-dsdt", t_other_dsdt }, { "other-table", t_other_table }, { "not-intel", t_not_intel },
	{ "bad-command", t_bad_command }, { "default-off", t_default_off }, { "on-off", t_on_off },
	{ "already-set", t_already_set }, { "ec-silent", t_ec_silent }, { "lock-busy", t_lock_busy },
	{ "write-fails", t_write_fails }, { "boot-flag", t_boot_flag }, { "boot-flag-0", t_boot_flag_zero },
	{ "bat-absent", t_bat_absent }, { "bat-absent-unknown", t_bat_absent_unknown },
	{ "bat-absent-ec-fails", t_bat_absent_ec_fails }, { "bat-never-read", t_bat_never_read },
	{ "bat-present", t_bat_present }, { "bat-present-no-full", t_bat_present_no_full },
	{ "bat-unplug", t_bat_unplug }, { "bat-seen-then-fails", t_bat_seen_then_fails },
};

int main(void)
{
	int bad = 0, n = (int)(sizeof(T) / sizeof(T[0]));
	for (int i = 0; i < n; i++) {
		fflush(stdout);
		pid_t p = fork();
		if (p == 0) { alarm(10); reset_model(); T[i].fn(); _exit(0); }   /* a hang is a failure */
		int st;
		waitpid(p, &st, 0);
		int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
		printf("%s %s\n", ok ? "ok  " : "FAIL", T[i].name);
		bad += !ok;
	}
	printf("casper_ec model: %d/%d\n", n - bad, n);
	return bad ? 1 : 0;
}
