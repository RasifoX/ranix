/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Sunrise Point register model for drivers/watchdog/itco.c. QEMU has no such
 * chipset, so this is the only way that path runs before real hardware does.
 *
 * Every starting value is a read-only dump from a real Casper Nirvana (i5-7200U,
 * Sunrise Point-LP, BIOS 215): SMBus/PMC config space, TCO and SMI_EN I/O values,
 * and the NO_REBOOT address Linux claims in /proc/iomem. The model enforces the
 * documented traps: P2SB reads as all ones while hidden, NMI_NOW must never be
 * written, TCO_LOCK is sticky, status bits are write-1-to-clear.
 *
 * Proves the driver's register sequence against those values. It cannot prove
 * what the silicon does with them.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stub/model.h"
#include "../../drivers/watchdog/itco.h"

#define TCO 0x400
#define PM  0x1800
#define REAL_NO_REBOOT_ADDR 0xFDC6000CULL   /* /proc/iomem, iTCO_wdt resource */

static const uint8_t smb_cfg[0x60] = {   /* lspci -xxx -s 00:1f.4 */
	0x86,0x80,0x23,0x9d,0x03,0x00,0x80,0x02,0x21,0x00,0x05,0x0c,0,0,0,0,
	0x04,0xe0,0x22,0xa4,0,0,0,0,0,0,0,0,0,0,0,0,
	0x41,0x60,0,0,0,0,0,0,0,0,0,0,0x0a,0x1b,0xc8,0x22,
	0,0,0,0,0,0,0,0,0,0,0,0,0x0b,0x01,0,0,
	0x11,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0x01,0x04,0,0,0,0x01,0,0,0,0,0,0,0,0,0,0,
};
static const uint8_t pmc_cfg[0x50] = {   /* lspci -xxx -s 00:1f.2 */
	0x86,0x80,0x21,0x9d,0x06,0,0,0,0x21,0,0x80,0x05,0,0,0x80,0,
	0,0x40,0x22,0xa4,0,0,0,0,0,0,0,0,0,0,0,0,
	0,0,0,0,0,0,0,0,0,0,0,0,0x0a,0x1b,0xc8,0x22,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	0x01,0x18,0,0,0x80,0x01,0,0,0,0,0,0xfe,0,0,0,0,
};

static struct {
	uint32_t smb_id;
	uint32_t tcoctl;
	int p2sb_hidden, p2sb_refuses, unhides, rehides;
	uint32_t gc;
	int gc_stuck, gc_reads, gc_writes, ioremaps;
	uint64_t ioremap_phys;
	uint16_t rld, sts1, sts2, cnt, tmr;
	int tco_floats, reloads;
	int tco_writes;
	uint32_t smi_en;
	char log[2048];
} m;
static uint32_t gc_cell;   /* the pointer ioremap hands back */

static void reset_model(void)
{
	memset(&m, 0, sizeof(m));
	m.smb_id = 0x9d238086;
	m.tcoctl = 0x100;
	m.p2sb_hidden = 1;
	m.gc  = 0x2;           /* assume the safe power-on state: NO_REBOOT set */
	m.rld = 0x4; m.cnt = 0x1800; m.tmr = 0x32;
	m.smi_en = 0x80002033;
}

static uint32_t cfg32(const uint8_t *c, size_t n, uint8_t reg)
{
	if ((size_t)reg + 4 > n) return 0;
	return (uint32_t)c[reg] | c[reg+1] << 8 | c[reg+2] << 16 | (uint32_t)c[reg+3] << 24;
}

uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t reg)
{
	assert(b == 0 && d == 0x1F);
	switch (f) {
	case 0: return reg == 0 ? 0x9d588086 : 0;
	case 1:
		if (m.p2sb_hidden) return 0xFFFFFFFF;
		if (reg == 0x00) return 0x9d208086;
		if (reg == 0x10) return 0xFD000004;
		return 0;
	case 2: return cfg32(pmc_cfg, sizeof(pmc_cfg), reg);
	case 4:
		if (reg == 0x00) return m.smb_id;
		if (reg == 0x54) return m.tcoctl;
		return cfg32(smb_cfg, sizeof(smb_cfg), reg);
	}
	return 0xFFFFFFFF;
}

void pci_write8(uint8_t b, uint8_t d, uint8_t f, uint8_t reg, uint8_t v)
{
	assert(b == 0 && d == 0x1F && f == 1 && reg == 0xE1);   /* nothing else, ever */
	if (v == 0) { m.unhides++; if (!m.p2sb_refuses) m.p2sb_hidden = 0; }
	else        { assert(v == 1); m.rehides++; m.p2sb_hidden = 1; }
}

uint16_t inw(uint16_t port)
{
	if (m.tco_floats) return 0xFFFF;
	switch (port - TCO) {
	case 0x00: return m.rld;
	case 0x04: return m.sts1;
	case 0x06: return m.sts2;
	case 0x08: return m.cnt;
	case 0x12: return m.tmr;
	}
	assert(!"read of an unexpected TCO register");
	return 0;
}

void outw(uint16_t port, uint16_t v)
{
	m.tco_writes++;
	switch (port - TCO) {
	case 0x00: m.rld = m.tmr & 0x3FF; m.reloads++; break;
	case 0x04: m.sts1 &= (uint16_t)~v; break;
	case 0x06: m.sts2 &= (uint16_t)~v; break;
	case 0x08:
		assert(!(v & 0x100));             /* a written 1 raises an NMI */
		m.cnt = (uint16_t)(v | (m.cnt & 0x1000));   /* TCO_LOCK is sticky */
		break;
	case 0x12: m.tmr = v; break;
	default: assert(!"write to an unexpected TCO register");
	}
}

uint32_t inl(uint16_t port)
{
	assert(port == PM + 0x30);
	return m.smi_en;
}

void *ioremap(uint64_t phys, size_t size, uint32_t flags)
{
	assert(flags & VM_NOCACHE);
	assert(size == 4);
	m.ioremaps++;
	m.ioremap_phys = phys;
	return &gc_cell;
}

uint32_t mmio_read32(volatile uint32_t *reg)
{
	assert(reg == &gc_cell);
	m.gc_reads++;
	return m.gc;
}

void mmio_write32(volatile uint32_t *reg, uint32_t v)
{
	assert(reg == &gc_cell);
	m.gc_writes++;
	assert(m.gc_reads > 0);               /* read-modify-write, never blind */
	m.gc = m.gc_stuck ? (m.gc | 2) : v;
}

void kprintf(const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	size_t n = strlen(m.log);
	vsnprintf(m.log + n, sizeof(m.log) - n, fmt, a);
	va_end(a);
}

static int passed;
#define PASS(name) do { printf("PASS %s\n", name); passed++; } while (0)

/* The driver keeps state in statics; each case runs in its own process. */
static void casper_arm_for_120s(void)
{
	reset_model();
	assert(itco_probe() == 0);
	assert(m.ioremap_phys == REAL_NO_REBOOT_ADDR);   /* the real address, derived */
	assert(m.gc == 0);                               /* NO_REBOOT cleared */
	assert(m.unhides == 1 && m.rehides == 1 && m.p2sb_hidden);
	assert(itco_fired_last_boot() == 0);

	assert(itco_start(100) == 0);
	assert((m.tmr & 0x3FF) == 100);
	assert(!(m.cnt & 0x800));                        /* running */
	assert(m.cnt & 0x1000);                          /* TCO_LOCK left set */
	assert(m.rld == 100);
	assert(strstr(m.log, "first expiry goes to SMM (SMI_EN=0x80002033, locked)"));

	int before = m.reloads;
	m.sts1 = 0x8;                                    /* a first expiry was taken */
	itco_pet();
	assert(m.reloads == before + 1 && m.sts1 == 0);

	itco_set_ticks(5000);
	assert((m.tmr & 0x3FF) == ITCO_TICKS_MAX);
	itco_set_ticks(0);
	assert((m.tmr & 0x3FF) == ITCO_TICKS_MIN);
	PASS("casper values: discovery, NO_REBOOT address, arm, pet, clamps");
}

static void gc_other_bits_preserved(void)
{
	reset_model();
	m.gc = 0xA5A50002;
	assert(itco_probe() == 0);
	assert(m.gc == 0xA5A50000);
	PASS("NO_REBOOT: neighbouring bits untouched");
}

static void gc_already_clear_no_write(void)
{
	reset_model();
	m.gc = 0;
	assert(itco_probe() == 0);
	assert(m.gc_writes == 0);
	PASS("NO_REBOOT already clear: no write");
}

static void strap_holds_noreboot(void)
{
	reset_model();
	m.gc_stuck = 1;
	assert(itco_probe() == -2);
	assert(m.p2sb_hidden);
	assert(itco_start(100) == -1);
	itco_pet();
	assert(m.reloads == 0 && (m.cnt & 0x800));       /* timer left halted */
	assert(strstr(m.log, "NO_REBOOT is stuck set"));
	PASS("strap holds NO_REBOOT: refused, timer never started");
}

static void p2sb_stays_hidden(void)
{
	reset_model();
	m.p2sb_refuses = 1;
	assert(itco_probe() == 0);
	assert(m.ioremaps == 0);                         /* no MMIO to a guessed base */
	assert(m.rehides == 1);
	assert(strstr(m.log, "NO_REBOOT state unknown"));
	PASS("P2SB will not unhide: no guessed MMIO, logged");
}

static void decode_off(void)
{
	reset_model();
	m.tcoctl = 0;
	assert(itco_probe() == -1);
	assert(m.unhides == 0 && m.ioremaps == 0 && m.tco_writes == 0);
	assert(strstr(m.log, "TCO decode off"));
	PASS("TCO_BASE_EN clear: no-op with a log line");
}

static void unknown_pch(void)
{
	reset_model();
	m.smb_id = 0xA3238086;                           /* a later PCH */
	assert(itco_probe() == -1);
	assert(m.unhides == 0 && m.ioremaps == 0 && m.tco_writes == 0);
	assert(strstr(m.log, "no supported Intel TCO"));
	PASS("unlisted SMBus id: nothing touched");
}

static void non_intel(void)
{
	reset_model();
	m.smb_id = 0x790b1022;                           /* AMD FCH SMBus */
	assert(itco_probe() == -1);
	assert(m.unhides == 0 && m.ioremaps == 0 && m.tco_writes == 0);
	PASS("non-Intel: nothing touched");
}

static void tco_port_floats(void)
{
	reset_model();
	m.tco_floats = 1;
	assert(itco_probe() == -1);
	assert(m.tco_writes == 0);
	assert(strstr(m.log, "does not decode"));
	PASS("TCO ports float: refused before any write");
}

static void nmi_now_never_written(void)
{
	reset_model();
	m.cnt |= 0x100;
	assert(itco_probe() == 0);
	assert(itco_start(100) == 0);                    /* outw() asserts on the bit */
	PASS("NMI_NOW set: never written back");
}

static void reports_previous_timeout(void)
{
	reset_model();
	m.sts2 = 0x2;
	assert(itco_probe() == 0);
	assert(itco_fired_last_boot() == 1);
	assert(itco_start(100) == 0);
	assert(m.sts2 == 0);
	PASS("SECOND_TO_STS: reported, then cleared");
}

#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
	void (*cases[])(void) = {
		casper_arm_for_120s, gc_other_bits_preserved, gc_already_clear_no_write,
		strap_holds_noreboot, p2sb_stays_hidden, decode_off, unknown_pch, non_intel,
		tco_port_floats, nmi_now_never_written, reports_previous_timeout,
	};
	int n = sizeof(cases) / sizeof(cases[0]), failed = 0;
	for (int i = 0; i < n; i++) {
		fflush(stdout);
		pid_t p = fork();
		if (p == 0) { cases[i](); fflush(stdout); _exit(0); }
		int st;
		waitpid(p, &st, 0);
		if (!WIFEXITED(st) || WEXITSTATUS(st)) { printf("FAIL case %d\n", i); failed++; }
	}
	printf("%d/%d passed\n", n - failed, n);
	return failed != 0;
}
