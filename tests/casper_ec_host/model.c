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
	int held, holds, multi;    /* lock depth, holds, holds that changed >1 byte */
	int busy, left, cmd_writes;
	int sleeps, ioremaps;
	uint64_t ioremap_phys;
	uint32_t ioremap_flags;
	int log[512][2], nlog;     /* every mailbox byte written: offset, value */
	void (*thread)(void);
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
	m.ioremaps++; m.ioremap_phys = phys; m.ioremap_flags = flags;
	assert(size == 0x80);
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
	assert(!strcmp(name, "casper_ec"));
	m.thread = fn;
	return 7;
}

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

static const struct { const char *name; void (*fn)(void); } T[] = {
	{ "other-board", t_other_board }, { "other-dsdt", t_other_dsdt }, { "other-table", t_other_table }, { "not-intel", t_not_intel },
	{ "bad-command", t_bad_command }, { "default-off", t_default_off }, { "on-off", t_on_off },
	{ "already-set", t_already_set }, { "ec-silent", t_ec_silent }, { "lock-busy", t_lock_busy },
	{ "write-fails", t_write_fails }, { "boot-flag", t_boot_flag }, { "boot-flag-0", t_boot_flag_zero },
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
