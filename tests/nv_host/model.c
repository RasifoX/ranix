/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* drivers/gpu/nvidia (nv_chip.c, nv_probe.c) against a PCI and MMIO model.
 * QEMU has no NVIDIA GPU, so this is where the probe runs before real hardware.
 *
 * The 940MX model starts from a real config space (a Casper Nirvana, lspci -xxx
 * -s 01:00.0 under Debian), with COMMAND and MSI enable cleared as a cold boot
 * leaves them. The model enforces what the hardware would punish or hide:
 * - BAR0 reads all ones unless the function is in D0 with memory decode on;
 * - a BAR may be sized only with memory decode off, and must be restored;
 * - leaving D3hot needs 10 ms (PCI PM 1.2) before the function is used;
 * - bus mastering must never be turned on, BAR1 must never be read;
 * - MMIO mappings must be uncached device memory.
 * Run: tests/nv_host/run.sh
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stub/nv_model.h"
#include "../../drivers/gpu/nvidia/nv_probe.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- PCI model: up to 4 functions ---- */
typedef struct {
	int      present;
	uint8_t  bus, dev, fn;
	uint8_t  cfg[256];
	uint64_t bar_size[6];      /* 0: not implemented */
	int      sizing[6];        /* all ones was written */
	int      off;              /* D3cold-like: MMIO reads all ones whatever the state */
	uint32_t boot0, boot42;
	uint8_t *bar0_mem;         /* model MMIO, 16 MiB */
	uint64_t pm_left_d3_at;    /* inb count when D3 -> D0 */
	int      left_d3;
} fnm_t;

static struct {
	fnm_t f[4];
	char  log[8192];
	long  inb_count;
	int   ioremaps, msi_attaches, master_set, sized_decoding, bar1_reads, early_reads;
	uint32_t ioremap_flags[4];
	uint64_t ioremap_phys[4], ioremap_size[4];
	irq_handler_t msi_handler;
	hwdiag_fn diag;
	void *other_maps[4];
} m;

static fnm_t *fnm(uint8_t b, uint8_t d, uint8_t f)
{
	for (int i = 0; i < 4; i++)
		if (m.f[i].present && m.f[i].bus == b && m.f[i].dev == d && m.f[i].fn == f) return &m.f[i];
	return NULL;
}

static uint32_t cfg32(fnm_t *x, uint8_t r) { uint32_t v; memcpy(&v, x->cfg + r, 4); return v; }
static void set32(fnm_t *x, uint8_t r, uint32_t v) { memcpy(x->cfg + r, &v, 4); }
static uint16_t cmd_of(fnm_t *x) { return (uint16_t)(x->cfg[4] | x->cfg[5] << 8); }
static int pm_state(fnm_t *x) { uint8_t pm = x->cfg[0x34]; return pm ? (x->cfg[pm + 4] & 3) : 0; }

/* the read of a BAR dword (index i = 0..5) */
static uint32_t bar_read(fnm_t *x, int i)
{
	uint32_t v = cfg32(x, (uint8_t)(0x10 + 4 * i));
	if (!x->sizing[i]) return v;
	/* the upper half of a 64-bit BAR? */
	if (i > 0 && x->bar_size[i - 1] && (cfg32(x, (uint8_t)(0x10 + 4 * (i - 1))) & 6) == 4)
		return (uint32_t)(~(x->bar_size[i - 1] - 1) >> 32);
	if (!x->bar_size[i]) return 0;
	return (uint32_t)~(x->bar_size[i] - 1) | (v & 0xF);
}

uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t r)
{
	fnm_t *x = fnm(b, d, f);
	if (!x) return 0xFFFFFFFF;
	assert((r & 3) == 0);
	if (r >= 0x10 && r < 0x28) return bar_read(x, (r - 0x10) / 4);
	return cfg32(x, r);
}

uint16_t pci_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t r)
{
	fnm_t *x = fnm(b, d, f);
	if (!x) return 0xFFFF;
	assert((r & 1) == 0);
	if (r >= 0x10 && r < 0x28) return (uint16_t)(bar_read(x, (r - 0x10) / 4) >> ((r & 2) * 8));
	return (uint16_t)(x->cfg[r] | x->cfg[r + 1] << 8);
}

void pci_write32(uint8_t b, uint8_t d, uint8_t f, uint8_t r, uint32_t v)
{
	fnm_t *x = fnm(b, d, f);
	assert(x && (r & 3) == 0);
	if (r >= 0x10 && r < 0x28) {
		int i = (r - 0x10) / 4;
		if (cmd_of(x) & PCI_CMD_MEM) m.sized_decoding++;
		if (v == 0xFFFFFFFF) { x->sizing[i] = 1; return; }
		x->sizing[i] = 0;
		set32(x, r, (v & ~0xFu) | (cfg32(x, r) & 0xF));
		return;
	}
	if (r == PCI_COMMAND) { pci_write16(b, d, f, r, (uint16_t)v); return; }
	set32(x, r, v);
}

void pci_write16(uint8_t b, uint8_t d, uint8_t f, uint8_t r, uint16_t v)
{
	fnm_t *x = fnm(b, d, f);
	assert(x && (r & 1) == 0);
	assert(!(r >= 0x10 && r < 0x28));
	uint8_t pm = x->cfg[0x34];
	if (r == PCI_COMMAND && (v & PCI_CMD_MASTER)) m.master_set++;
	if (pm && r == pm + 4 && (x->cfg[r] & 3) && !(v & 3)) { x->pm_left_d3_at = (uint64_t)m.inb_count; x->left_d3 = 1; }
	x->cfg[r] = (uint8_t)v; x->cfg[r + 1] = (uint8_t)(v >> 8);
}

uint8_t pci_find_cap(uint8_t b, uint8_t d, uint8_t f, uint8_t id)
{
	fnm_t *x = fnm(b, d, f);
	if (!x || !(x->cfg[6] & 0x10)) return 0;
	for (uint8_t p = x->cfg[0x34] & 0xFC; p; p = x->cfg[p + 1] & 0xFC)
		if (x->cfg[p] == id) return p;
	return 0;
}

int pci_msi_attach(uint8_t b, uint8_t d, uint8_t f, irq_handler_t h)
{
	fnm_t *x = fnm(b, d, f);
	assert(x && h);
	m.msi_attaches++;
	m.msi_handler = h;
	if (!pci_find_cap(b, d, f, PCI_CAP_MSI)) return -1;
	return 0x31;
}

void *ioremap(uint64_t phys, size_t size, uint32_t flags)
{
	assert(m.ioremaps < 4);
	m.ioremap_phys[m.ioremaps] = phys;
	m.ioremap_size[m.ioremaps] = size;
	m.ioremap_flags[m.ioremaps++] = flags;
	for (int i = 0; i < 4; i++) {
		fnm_t *x = &m.f[i];
		if (!x->present || !x->bar0_mem) continue;
		if (phys == (cfg32(x, 0x10) & ~0xFu)) { assert(size <= 16u << 20); return x->bar0_mem; }
	}
	return m.other_maps[m.ioremaps - 1] = calloc(1, size);   /* BAR1: never read (mmio_read32 checks) */
}

uint8_t inb(uint16_t port) { assert(port == 0x80); m.inb_count++; return 0; }

uint32_t mmio_read32(volatile uint32_t *reg)
{
	for (int i = 0; i < 4; i++) {
		fnm_t *x = &m.f[i];
		if (!x->present || !x->bar0_mem) continue;
		uint8_t *p = (uint8_t *)reg;
		if (p < x->bar0_mem || p >= x->bar0_mem + (16u << 20)) continue;
		size_t off = (size_t)(p - x->bar0_mem);
		if (x->off || pm_state(x) != 0 || !(cmd_of(x) & PCI_CMD_MEM)) return 0xFFFFFFFF;
		if (x->left_d3 && (uint64_t)m.inb_count - x->pm_left_d3_at < 10000) { m.early_reads++; return 0xFFFFFFFF; }
		if (off == 0) return x->boot0;
		if (off == 0xA00) return x->boot42;
		return 0xBADF1000;   /* what an unimplemented register reads as */
	}
	m.bar1_reads++;
	return 0;
}

void kprintf(const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	size_t n = strlen(m.log);
	vsnprintf(m.log + n, sizeof(m.log) - n, fmt, a);
	va_end(a);
}

void hw_begin(hw_out_t *o, const char *cls, const char *name) { int n = snprintf(o->p, o->rem, "%s %s", cls, name); o->p += n; o->rem -= (size_t)n; }
void hw_kv(hw_out_t *o, const char *k, uint64_t v) { int n = snprintf(o->p, o->rem, " %s=%llu", k, (unsigned long long)v); o->p += n; o->rem -= (size_t)n; }
void hw_ks(hw_out_t *o, const char *k, const char *s) { assert(!strchr(s, ' ')); int n = snprintf(o->p, o->rem, " %s=%s", k, s); o->p += n; o->rem -= (size_t)n; }
void hw_end(hw_out_t *o) { int n = snprintf(o->p, o->rem, "\n"); o->p += n; o->rem -= (size_t)n; }
void hwdiag_register(hwdiag_fn fn) { assert(!m.diag); m.diag = fn; }

static const char *diag_line(void)
{
	static char b[512];
	b[0] = 0;
	if (!m.diag) return b;
	hw_out_t o = { b, sizeof b };
	m.diag(&o);
	return b;
}

/* ---- machines ---- */

/* Casper 01:00.0 GeForce 940MX, lspci -xxx (Debian, nvidia 535 bound), 0x00-0xbf */
static const uint8_t mx940_cfg[0xC0] = {
	0xde,0x10,0x4d,0x13,0x07,0x04,0x10,0x00,0xa2,0x00,0x02,0x03,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0xa3,0x0c,0x00,0x00,0x90,0x00,0x00,0x00,0x00,0x0c,0x00,0x00,0xa0,
	0x00,0x00,0x00,0x00,0x01,0x50,0x00,0x00,0x00,0x00,0x00,0x00,0x0a,0x1b,0x12,0x23,
	0x00,0x00,0x00,0x00,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0b,0x01,0x00,0x00,
	0x0a,0x1b,0x12,0x23,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0xce,0xd6,0x23,0x00,0x00,0x00,0x00,0x00,
	0x01,0x68,0x03,0x00,0x08,0x00,0x00,0x00,0x05,0x78,0x81,0x00,0x58,0x03,0xe0,0xfe,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x00,0x02,0x00,0xe1,0x8d,0xe8,0x07,
	0x30,0x21,0x00,0x00,0x43,0xbc,0x47,0x00,0x40,0x01,0x41,0x10,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x13,0x08,0x04,0x00,
	0x00,0x04,0x00,0x00,0x0e,0x00,0x00,0x00,0x03,0x00,0x1e,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x09,0x00,0x14,0x01,0x00,0x00,0x10,0x80,0x00,0x00,0x00,0x00,
};

static void reset(void)
{
	for (int i = 0; i < 4; i++) { free(m.f[i].bar0_mem); free(m.other_maps[i]); }
	memset(&m, 0, sizeof m);
}

static fnm_t *add(uint8_t b, uint8_t d, uint8_t f)
{
	for (int i = 0; i < 4; i++)
		if (!m.f[i].present) { fnm_t *x = &m.f[i]; x->present = 1; x->bus = b; x->dev = d; x->fn = f; return x; }
	assert(0);
	return NULL;
}

static fnm_t *add_940mx(void)
{
	fnm_t *x = add(1, 0, 0);
	memcpy(x->cfg, mx940_cfg, sizeof mx940_cfg);
	x->cfg[4] = 0; x->cfg[5] = 0;               /* COMMAND as a cold boot leaves it */
	x->cfg[0x6a] = 0x80;                        /* MSI control: 64-bit, not enabled */
	x->bar_size[0] = 16u << 20;                 /* lspci: Region 0 16M, 1 256M (64-bit), 3 32M, 5 I/O 128 */
	x->bar_size[1] = 256u << 20;
	x->bar_size[3] = 32u << 20;
	x->bar_size[5] = 128;
	x->boot0 = 0x118010a2;                      /* GM108 rev A2 */
	x->boot42 = 0xBADF1000;
	x->bar0_mem = calloc(1, 16u << 20);
	return x;
}

static void add_intel_vga(void)
{
	fnm_t *x = add(0, 2, 0);
	set32(x, 0x00, 0x59168086); set32(x, 0x08, 0x03000002);
	set32(x, 0x10, 0xa2000004); x->bar_size[0] = 16u << 20;
}

/* ---- cases ---- */

static void t_pci_match(void)
{
	CHECK(nv_pci_is_gpu(0x134d10de, 0x030200a2) == 1, "940MX 3D controller");
	CHECK(nv_pci_is_gpu(0x28e110de, 0x030000a1) == 1, "RTX 4050 Laptop as VGA");
	CHECK(nv_pci_is_gpu(0x28e110de, 0x038000a1) == 1, "display controller, other");
	CHECK(nv_pci_is_gpu(0x22be10de, 0x040300a1) == 0, "the card's HD audio is not a GPU");
	CHECK(nv_pci_is_gpu(0x59168086, 0x03000002) == 0, "Intel HD 620 is not ours");
	CHECK(nv_pci_is_gpu(0xffffffff, 0xffffffff) == 0, "no function");
}

static void t_boot0(void)
{
	nv_chip_t c;
	CHECK(nv_boot0_decode(0x118010a2, &c) == 0, "GM108 decodes");
	CHECK(c.chip == 0x118 && c.arch == 0x11 && c.impl == 8, "GM108 fields %x %x %x", c.chip, c.arch, c.impl);
	CHECK(c.rev_major == 0xA && c.rev_minor == 2, "rev A2: %x %x", c.rev_major, c.rev_minor);
	CHECK(!strcmp(c.name, "GM108") && !strcmp(c.family, "Maxwell") && c.open_modules == 0, "GM108 %s %s %d", c.name, c.family, c.open_modules);
	CHECK(nv_boot0_decode(0x197000a1, &c) == 0 && !strcmp(c.name, "AD107") && !strcmp(c.family, "Ada") && c.open_modules == 1, "AD107 %s %s", c.name, c.family);
	CHECK(nv_boot0_decode(0x124000a1, &c) == 0 && !strcmp(c.name, "GM204"), "GM204 %s", c.name);
	CHECK(nv_boot0_decode(0x137000a1, &c) == 0 && !strcmp(c.name, "GP107") && !strcmp(c.family, "Pascal"), "GP107 %s", c.name);
	CHECK(nv_boot0_decode(0x164000a1, &c) == 0 && !strcmp(c.name, "TU104") && c.open_modules == 1, "TU104 %s", c.name);
	CHECK(nv_boot0_decode(0x167000a1, &c) == 0 && !strcmp(c.name, "TU117"), "TU117 %s", c.name);
	CHECK(nv_boot0_decode(0x168000a1, &c) == 0 && !strcmp(c.name, "TU116"), "TU116 %s", c.name);
	CHECK(nv_boot0_decode(0x172000a1, &c) == 0 && !strcmp(c.name, "GA102") && !strcmp(c.family, "Ampere"), "GA102 %s", c.name);
	CHECK(nv_boot0_decode(0x0e4000a1, &c) == 0 && !strcmp(c.name, "GK104") && !strcmp(c.family, "Kepler"), "GK104 %s", c.name);
	CHECK(nv_boot0_decode(0x108000a1, &c) == 0 && !strcmp(c.name, "GK208"), "GK208 %s", c.name);
	CHECK(nv_boot0_decode(0x1b2000a1, &c) == 0 && !strcmp(c.family, "Blackwell") && c.open_modules == 1, "GB202 %s", c.family);
	CHECK(nv_boot0_decode(0x1f0000a1, &c) == 0 && c.name[0] == 0 && !strcmp(c.family, "unknown") && c.open_modules == 1, "a future arch: no name, still Turing+");
	CHECK(nv_boot0_decode(0xFFFFFFFF, &c) == -1, "all ones: off");
	CHECK(nv_boot0_decode(0, &c) == -2, "zero: not an id");
	CHECK(nv_boot0_decode(0x04000000, &c) == -2, "pre-Fermi: not ours");
}

static void t_bar(void)
{
	uint64_t base; int is64, pf;
	CHECK(nv_bar_decode(0xa3000000, 0, &base, &is64, &pf) == 0 && base == 0xa3000000 && !is64 && !pf, "BAR0 32-bit");
	CHECK(nv_bar_decode(0x9000000c, 0, &base, &is64, &pf) == 0 && base == 0x90000000 && is64 && pf, "BAR1 64-bit pref");
	CHECK(nv_bar_decode(0x0000000c, 0x4, &base, &is64, &pf) == 0 && base == 0x400000000ULL, "BAR above 4G");
	CHECK(nv_bar_decode(0x00005001, 0, &base, &is64, &pf) == -1, "I/O BAR");
	CHECK(nv_bar_size(0xff000000, 0, 0) == 16u << 20, "16M");
	CHECK(nv_bar_size(0xf000000c, 0xffffffff, 1) == 256u << 20, "256M 64-bit");
	CHECK(nv_bar_size(0x0000000c, 0xfffffffe, 1) == 8ULL << 30, "8G resizable");
	CHECK(nv_bar_size(0, 0, 0) == 0, "not implemented");
	char b[16];
	CHECK(!strcmp(nv_size_str(16u << 20, b), "16M") && !strcmp(nv_size_str(8ULL << 30, b), "8G") && !strcmp(nv_size_str(4096, b), "4K"), "size strings");
}

static void t_940mx(void)
{
	reset();
	add_intel_vga();
	fnm_t *x = add_940mx();
	nv_probe_init();
	const nv_gpu_t *g = nv_gpu();
	CHECK(g->state == NV_ST_UP, "state %d, log: %s", g->state, m.log);
	CHECK(g->bus == 1 && g->dev == 0 && g->fn == 0 && g->device == 0x134d, "found at 01:00.0");
	CHECK(!strcmp(g->chip.name, "GM108") && g->chip.boot0 == 0x118010a2, "chip %s", g->chip.name);
	CHECK(g->bar0 == 0xa3000000 && g->bar0_size == 16u << 20, "BAR0 %llx %llx", (unsigned long long)g->bar0, (unsigned long long)g->bar0_size);
	CHECK(g->bar1 == 0x90000000 && g->bar1_size == 256u << 20 && g->bar1_mapped == 16u << 20, "BAR1");
	CHECK(cfg32(x, 0x10) == 0xa3000000 && cfg32(x, 0x14) == 0x9000000c && cfg32(x, 0x18) == 0, "BARs restored after sizing");
	CHECK(!x->sizing[0] && !x->sizing[1] && !x->sizing[2], "no BAR left in sizing mode");
	CHECK(m.sized_decoding == 0, "BARs sized with memory decode on (%d)", m.sized_decoding);
	CHECK(cmd_of(x) & PCI_CMD_MEM, "memory decode on");
	CHECK(m.master_set == 0 && !(cmd_of(x) & PCI_CMD_MASTER), "bus mastering stays off");
	CHECK(m.ioremaps == 2, "two mappings (%d)", m.ioremaps);
	for (int i = 0; i < m.ioremaps; i++) {
		CHECK((m.ioremap_flags[i] & (VM_NOCACHE | VM_DEVICE)) == (VM_NOCACHE | VM_DEVICE) && !(m.ioremap_flags[i] & VM_WC), "mapping %d uncached", i);
	}
	CHECK(m.bar1_reads == 0, "BAR1 read %d times", m.bar1_reads);
	CHECK(g->msi_vec == 0x31 && m.msi_attaches == 1, "MSI vector %d", g->msi_vec);
	CHECK(g->boot42 == 0, "BOOT_42 not read on Maxwell");
	CHECK(strstr(m.log, "01:00.0") && strstr(m.log, "GM108") && strstr(m.log, "Maxwell") && strstr(m.log, "118010a2"), "log: %s", m.log);
	CHECK(strstr(m.log, "MSI vector 49"), "log names the vector: %s", m.log);
	CHECK(strstr(m.log, "open modules: no"), "log says open modules do not cover Maxwell: %s", m.log);
	m.msi_handler(); m.msi_handler();
	CHECK(g->irqs == 2, "handler counts");
	const char *l = diag_line();
	CHECK(strstr(l, "gpu nvidia0 ") && strstr(l, "chip=GM108") && strstr(l, "boot0=293605538") && strstr(l, "msi_vec=49") && strstr(l, "irqs=2") && strstr(l, "state=up"), "diag: %s", l);
}

static void t_d3hot(void)
{
	reset();
	fnm_t *x = add_940mx();
	x->cfg[0x64] = 3;                          /* PMCSR: D3hot */
	nv_probe_init();
	const nv_gpu_t *g = nv_gpu();
	CHECK(g->pm_before == 3, "saw D3hot (%d)", g->pm_before);
	CHECK(pm_state(x) == 0, "brought to D0");
	CHECK(m.early_reads == 0, "BOOT_0 read before 10 ms in D0 (%d)", m.early_reads);
	CHECK(g->state == NV_ST_UP && !strcmp(g->chip.name, "GM108"), "identified after D3hot: %d %s", g->state, m.log);
}

static void t_off(void)
{
	reset();
	fnm_t *x = add_940mx();
	x->off = 1;
	nv_probe_init();
	const nv_gpu_t *g = nv_gpu();
	CHECK(g->state == NV_ST_OFF, "state %d", g->state);
	CHECK(m.msi_attaches == 0, "no MSI for a GPU that does not answer");
	CHECK(strstr(m.log, "does not answer"), "log: %s", m.log);
	CHECK(strstr(diag_line(), "state=off"), "diag: %s", diag_line());
}

static void t_none(void)
{
	reset();
	add_intel_vga();
	fnm_t *a = add(1, 0, 1);                    /* an NVIDIA HD audio function alone */
	set32(a, 0x00, 0x0fbc10de); set32(a, 0x08, 0x040300a1);
	nv_probe_init();
	CHECK(nv_gpu()->state == NV_ST_NONE, "nothing found");
	CHECK(m.ioremaps == 0 && m.msi_attaches == 0 && m.log[0] == 0 && !m.diag, "silent: %s", m.log);
}

static void t_nobar(void)
{
	reset();
	fnm_t *x = add_940mx();
	set32(x, 0x10, 0);
	nv_probe_init();
	CHECK(nv_gpu()->state == NV_ST_NOBAR && m.ioremaps == 0 && m.msi_attaches == 0, "unassigned BAR0 left alone (%d)", nv_gpu()->state);
}

static void t_ada(void)
{
	reset();
	fnm_t *x = add(1, 0, 0);
	memcpy(x->cfg, mx940_cfg, sizeof mx940_cfg);
	set32(x, 0x00, 0x28e110de); set32(x, 0x08, 0x030000a1);
	x->cfg[4] = 0; x->cfg[5] = 0; x->cfg[0x6a] = 0x80;
	set32(x, 0x14, 0x0000000c); set32(x, 0x18, 0x00000004);   /* BAR1 at 16G, 8G (resizable BAR) */
	x->bar_size[0] = 16u << 20; x->bar_size[1] = 8ULL << 30; x->bar_size[3] = 32u << 20; x->bar_size[5] = 128;
	x->boot0 = 0x197000a1; x->boot42 = 0x197000a1;
	x->bar0_mem = calloc(1, 16u << 20);
	nv_probe_init();
	const nv_gpu_t *g = nv_gpu();
	CHECK(g->state == NV_ST_UP && !strcmp(g->chip.name, "AD107") && g->chip.open_modules, "AD107: %s", m.log);
	CHECK(g->bar1 == 0x400000000ULL && g->bar1_size == 8ULL << 30 && g->bar1_mapped == NV_BAR1_MAP_MAX, "BAR1 8G, 16M mapped");
	CHECK(cfg32(x, 0x14) == 0x0000000c && cfg32(x, 0x18) == 4, "64-bit BAR1 restored");
	CHECK(g->boot42 == 0x197000a1, "BOOT_42 read on Ada");
	CHECK(strstr(m.log, "open modules: yes"), "log: %s", m.log);
}

int main(void)
{
	t_pci_match();
	t_boot0();
	t_bar();
	t_940mx();
	t_d3hot();
	t_off();
	t_none();
	t_nobar();
	t_ada();
	reset();
	printf("nv_host: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
