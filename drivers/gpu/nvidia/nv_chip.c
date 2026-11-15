/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* NVIDIA GPU identification: pure decode, no hardware access. Sources: nv_chip.h. */
#include "nv_chip.h"

int nv_pci_is_gpu(uint32_t id, uint32_t class_rev)
{
	if ((id & 0xFFFF) != NV_VENDOR) return 0;
	uint8_t cls = (uint8_t)(class_rev >> 24), sub = (uint8_t)(class_rev >> 16);
	return cls == 0x03 && (sub == 0x00 || sub == 0x02 || sub == 0x80);
}

/* ARCHITECTURE -> family and the name's prefix and generation digits:
 * chip 0x118 = arch 0x11 "GM" "10" + impl 8 = GM108. open-gpu-doc gives the
 * values up to 0x17; the names follow envytools' chipset list (see nv_chip.h). */
static const struct { uint8_t arch; const char *family, *prefix; } arch_tab[] = {
	{ 0x0C, "Fermi",     "GF10" }, { 0x0D, "Fermi",    "GF11" },
	{ 0x0E, "Kepler",    "GK10" }, { 0x0F, "Kepler",   "GK11" }, { 0x10, "Kepler", "GK20" },
	{ 0x11, "Maxwell",   "GM10" }, { 0x12, "Maxwell",  "GM20" },
	{ 0x13, "Pascal",    "GP10" },
	{ 0x14, "Volta",     "GV10" }, { 0x15, "Volta",    "GV11" },
	{ 0x16, "Turing",    "TU10" },
	{ 0x17, "Ampere",    "GA10" },
	{ 0x18, "Hopper",    "GH10" },
	{ 0x19, "Ada",       "AD10" },
	{ 0x1A, "Blackwell", "GB10" }, { 0x1B, "Blackwell", "GB20" },
};

int nv_boot0_decode(uint32_t boot0, nv_chip_t *c)
{
	c->boot0     = boot0;
	c->arch      = (uint8_t)((boot0 >> 24) & 0x1F);
	c->impl      = (uint8_t)((boot0 >> 20) & 0xF);
	c->chip      = (uint16_t)((boot0 >> 20) & 0x1FF);
	c->rev_major = (uint8_t)((boot0 >> 4) & 0xF);
	c->rev_minor = (uint8_t)(boot0 & 0xF);
	c->family    = "unknown";
	c->name[0]   = 0;
	c->open_modules = 0;
	if (boot0 == 0xFFFFFFFF) return -1;
	if (c->arch < 0x0C) return -2;          /* 0, or older than Fermi */
	c->open_modules = c->arch >= NV_ARCH_TURING;

	for (unsigned i = 0; i < sizeof arch_tab / sizeof arch_tab[0]; i++) {
		if (arch_tab[i].arch != c->arch) continue;
		c->family = arch_tab[i].family;
		const char *p = arch_tab[i].prefix;
		int n = 0;
		while (p[n]) { c->name[n] = p[n]; n++; }
		c->name[n++] = "0123456789ABCDEF"[c->impl];
		c->name[n] = 0;
		/* Turing's small chips break the pattern: 0x167 is TU117, 0x168 TU116 */
		if (c->arch == 0x16 && c->impl == 7) { c->name[3] = '1'; c->name[4] = '7'; }
		if (c->arch == 0x16 && c->impl == 8) { c->name[3] = '1'; c->name[4] = '6'; }
		break;
	}
	return 0;
}

int nv_bar_decode(uint32_t lo, uint32_t hi, uint64_t *base, int *is64, int *prefetch)
{
	if (lo & 1) return -1;                         /* I/O space */
	*is64 = (lo & 0x6) == 0x4;
	*prefetch = (lo & 0x8) != 0;
	*base = (uint64_t)(lo & ~0xFu);
	if (*is64) *base |= (uint64_t)hi << 32;
	return 0;
}

uint64_t nv_bar_size(uint32_t lo_mask, uint32_t hi_mask, int is64)
{
	uint64_t mask = (uint64_t)(lo_mask & ~0xFu);
	if (is64) mask |= (uint64_t)hi_mask << 32;
	else if (mask) mask |= 0xFFFFFFFF00000000ULL;
	if (!mask) return 0;
	return ~mask + 1;
}

const char *nv_size_str(uint64_t bytes, char *buf)
{
	static const char unit[] = "KMGT";
	int u = -1;
	uint64_t v = bytes;
	while (v >= 1024 && (v % 1024) == 0 && u < 3) { v /= 1024; u++; }
	char t[24]; int n = 0;
	do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	int k = 0;
	while (n) buf[k++] = t[--n];
	if (u >= 0) buf[k++] = unit[u];
	buf[k] = 0;
	return buf;
}
