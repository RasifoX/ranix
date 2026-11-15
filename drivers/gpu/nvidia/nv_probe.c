/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* NVIDIA GPU probe (see nv_probe.h): PCI, power state, BARs, NV_PMC_BOOT_0, MSI.
 *
 * Ranix's own code. No NVIDIA driver or Linux code is used; register facts and
 * their sources are in nv_chip.h. PCI facts: PCI Local Bus Specification 3.0
 * (6.2.2 Command register, 6.2.5.1 BAR sizing with decode off) and PCI Bus Power
 * Management Interface Specification 1.2 (PMCSR PowerState, 10 ms D3hot -> D0).
 *
 * What it does not do, on purpose: turn on bus mastering, enable any interrupt
 * inside the GPU (NV_PMC_INTR_EN stays as the firmware left it, so the MSI vector
 * stays quiet), read BAR1 (its contents need the GPU's BAR1 page tables), or power
 * the GPU up through ACPI. A hybrid laptop's dGPU that ACPI has switched off
 * (D3cold) either vanishes from config space (then nothing is found) or reads all
 * ones through BAR0 (then this says so and stops): running its _PS0/_ON method is
 * a separate step, as Ranix's AML interpreter is limited.
 */
#include <uapi/types.h>
#include <kernel/pci.h>
#include <kernel/kprintf.h>
#include <kernel/mm/vm.h>
#include <kernel/irq.h>
#include <kernel/hwdiag.h>
#include <asm/io.h>
#include "nv_probe.h"

#define PCI_REV_CLASS   0x08
#define PM_CSR          0x04            /* from the PM capability */
#define PM_STATE_MASK   0x0003

static nv_gpu_t g_nv;

const nv_gpu_t *nv_gpu(void) { return &g_nv; }

/* A rough delay: one port 0x80 read takes about 1 us and needs no timer. */
static void nv_udelay(uint32_t us)
{
	while (us--)
		(void)inb(0x80);
}

static void nv_irq(void)
{
	g_nv.irqs++;
}

static const char *state_word(int s)
{
	switch (s) {
	case NV_ST_UP:    return "up";
	case NV_ST_OFF:   return "off";
	case NV_ST_BADID: return "badid";
	case NV_ST_NOBAR: return "nobar";
	default:          return "none";
	}
}

/* /proc/hwdiag: gpu nvidia0 bdf=01:00.0 ... */
static void nv_diag(hw_out_t *o)
{
	nv_gpu_t *g = &g_nv;
	char bdf[8];
	bdf[0] = "0123456789abcdef"[g->bus >> 4]; bdf[1] = "0123456789abcdef"[g->bus & 15];
	bdf[2] = ':';
	bdf[3] = "0123456789abcdef"[g->dev >> 4]; bdf[4] = "0123456789abcdef"[g->dev & 15];
	bdf[5] = '.'; bdf[6] = (char)('0' + g->fn); bdf[7] = 0;
	hw_begin(o, "gpu", "nvidia0");
	hw_ks(o, "bdf", bdf);
	hw_kv(o, "vendor", NV_VENDOR);
	hw_kv(o, "device", g->device);
	hw_ks(o, "state", state_word(g->state));
	hw_kv(o, "pm_before", (uint64_t)(g->pm_before < 0 ? 9 : g->pm_before));
	if (g->state == NV_ST_UP || g->state == NV_ST_BADID) hw_kv(o, "boot0", g->chip.boot0);
	if (g->state == NV_ST_UP) {
		hw_ks(o, "chip", g->chip.name[0] ? g->chip.name : "unknown");
		hw_ks(o, "family", g->chip.family);
		hw_kv(o, "chip_id", g->chip.chip);
		hw_kv(o, "open_modules", (uint64_t)g->chip.open_modules);
	}
	hw_kv(o, "bar0_size", g->bar0_size);
	hw_kv(o, "bar1_size", g->bar1_size);
	hw_kv(o, "msi_vec", (uint64_t)(g->msi_vec < 0 ? 0 : g->msi_vec));
	hw_kv(o, "irqs", g->irqs);
	hw_end(o);
}

/* The first NVIDIA display function, scanning every function (the GPU is
 * function 0, its HD audio function 1, which is not a GPU). */
static int nv_find(nv_gpu_t *g)
{
	for (int b = 0; b < 256; b++)
		for (int d = 0; d < 32; d++)
			for (int f = 0; f < 8; f++) {
				uint32_t id = pci_read32((uint8_t)b, (uint8_t)d, (uint8_t)f, PCI_VENDOR_ID);
				if ((id & 0xFFFF) == 0xFFFF) { if (f == 0) break; continue; }
				uint32_t cr = pci_read32((uint8_t)b, (uint8_t)d, (uint8_t)f, PCI_REV_CLASS);
				if (!nv_pci_is_gpu(id, cr)) continue;
				g->bus = (uint8_t)b; g->dev = (uint8_t)d; g->fn = (uint8_t)f;
				g->device = (uint16_t)(id >> 16);
				g->pci_class = (uint8_t)(cr >> 24);
				g->pci_sub = (uint8_t)(cr >> 16);
				g->pci_rev = (uint8_t)cr;
				return 0;
			}
	return -1;
}

/* D0, as PCI PM requires before the BARs are used; 10 ms after D3hot. */
static void nv_to_d0(nv_gpu_t *g)
{
	uint8_t pm = pci_find_cap(g->bus, g->dev, g->fn, PCI_CAP_PM);
	g->pm_before = -1;
	if (!pm) return;
	uint16_t csr = pci_read16(g->bus, g->dev, g->fn, (uint8_t)(pm + PM_CSR));
	g->pm_before = csr & PM_STATE_MASK;
	if (g->pm_before) {
		pci_write16(g->bus, g->dev, g->fn, (uint8_t)(pm + PM_CSR), (uint16_t)(csr & ~PM_STATE_MASK));
		nv_udelay(20000);
	}
}

/* One memory BAR (reg = 0x10 + 4n): base and size. Memory decode must be off. */
static int nv_bar(nv_gpu_t *g, uint8_t reg, uint64_t *base, uint64_t *size, int *is64)
{
	int pf;
	uint32_t lo = pci_read32(g->bus, g->dev, g->fn, reg);
	if (lo & 1) return -1;
	int wide = (lo & 0x6) == 0x4;
	uint32_t hi = wide ? pci_read32(g->bus, g->dev, g->fn, (uint8_t)(reg + 4)) : 0;
	if (nv_bar_decode(lo, hi, base, is64, &pf) != 0) return -1;

	pci_write32(g->bus, g->dev, g->fn, reg, 0xFFFFFFFF);
	uint32_t lo_m = pci_read32(g->bus, g->dev, g->fn, reg);
	uint32_t hi_m = 0;
	if (wide) {
		pci_write32(g->bus, g->dev, g->fn, (uint8_t)(reg + 4), 0xFFFFFFFF);
		hi_m = pci_read32(g->bus, g->dev, g->fn, (uint8_t)(reg + 4));
	}
	pci_write32(g->bus, g->dev, g->fn, reg, lo);
	if (wide) pci_write32(g->bus, g->dev, g->fn, (uint8_t)(reg + 4), hi);
	*size = nv_bar_size(lo_m, hi_m, wide);
	return 0;
}

void nv_probe_init(void)
{
	nv_gpu_t *g = &g_nv;
	char s0[16], s1[16], s2[16];

	*g = (nv_gpu_t){ .state = NV_ST_NONE, .msi_vec = -1 };
	if (nv_find(g) != 0) return;     /* no NVIDIA GPU: the normal case, stay silent */
	hwdiag_register(nv_diag);

	nv_to_d0(g);

	/* Size BAR0 and BAR1 with decode off (PCI 3.0 6.2.5.1), then decode memory.
	 * Bus mastering is left as it was: nothing here lets the GPU access memory. */
	uint16_t cmd = pci_read16(g->bus, g->dev, g->fn, PCI_COMMAND);
	pci_write16(g->bus, g->dev, g->fn, PCI_COMMAND, (uint16_t)(cmd & ~(PCI_CMD_MEM | PCI_CMD_IO)));
	int w0 = 0, w1 = 0;
	int r0 = nv_bar(g, PCI_BAR0, &g->bar0, &g->bar0_size, &w0);
	int r1 = nv_bar(g, PCI_BAR1, &g->bar1, &g->bar1_size, &w1);
	pci_write16(g->bus, g->dev, g->fn, PCI_COMMAND, (uint16_t)(cmd | PCI_CMD_MEM));

	if (r0 != 0 || !g->bar0 || g->bar0_size < 0x1000) {
		g->state = NV_ST_NOBAR;
		kprintf(KERN_WARN "nvidia: %02x:%02x.%u [10de:%04x]: BAR0 not usable (base 0x%lx), left alone\n",
		        g->bus, g->dev, g->fn, g->device, (unsigned long)g->bar0);
		return;
	}

	g->regs = (volatile uint32_t *)ioremap(g->bar0, (size_t)g->bar0_size,
	                                        VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
	if (!g->regs) {
		g->state = NV_ST_NOBAR;
		kprintf(KERN_ERR "nvidia: ioremap of BAR0 failed\n");
		return;
	}
	if (r1 == 0 && g->bar1 && g->bar1_size) {
		g->bar1_mapped = g->bar1_size < NV_BAR1_MAP_MAX ? g->bar1_size : NV_BAR1_MAP_MAX;
		g->bar1_va = ioremap(g->bar1, (size_t)g->bar1_mapped, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
		if (!g->bar1_va) g->bar1_mapped = 0;
	}

	uint32_t boot0 = mmio_read32(g->regs + NV_PMC_BOOT_0 / 4);
	int rc = nv_boot0_decode(boot0, &g->chip);
	if (rc == -1) {
		g->state = NV_ST_OFF;
		kprintf(KERN_WARN "nvidia: %02x:%02x.%u [10de:%04x]: BOOT_0 reads all ones, the GPU does not answer "
		        "(powered off or D3cold; ACPI _PS0/_ON is not run by Ranix), left alone\n",
		        g->bus, g->dev, g->fn, g->device);
		return;
	}
	if (rc != 0) {
		g->state = NV_ST_BADID;
		kprintf(KERN_WARN "nvidia: %02x:%02x.%u [10de:%04x]: BOOT_0 0x%08x is no architecture we know, left alone\n",
		        g->bus, g->dev, g->fn, g->device, boot0);
		return;
	}
	if (g->chip.arch >= NV_ARCH_TURING)
		g->boot42 = mmio_read32(g->regs + NV_PMC_BOOT_42 / 4);
	g->state = NV_ST_UP;

	/* An MSI vector, so the path exists for the driver that will follow. The GPU
	 * is not asked to raise anything, so the count should stay 0. */
	g->msi_vec = pci_msi_attach(g->bus, g->dev, g->fn, nv_irq);

	kprintf(KERN_INFO "nvidia: %02x:%02x.%u [10de:%04x] class %02x%02x rev %02x: %s (%s, BOOT_0 0x%08x, chip 0x%x rev %c%x)\n",
	        g->bus, g->dev, g->fn, g->device, g->pci_class, g->pci_sub, g->pci_rev,
	        g->chip.name[0] ? g->chip.name : "unknown chip", g->chip.family, boot0, g->chip.chip,
	        (char)('A' + (g->chip.rev_major >= 0xA ? g->chip.rev_major - 0xA : 0)), g->chip.rev_minor);
	kprintf(KERN_INFO "nvidia: BAR0 0x%lx %s, BAR1 0x%lx %s (%s mapped), uncached; %s%d; open modules: %s\n",
	        (unsigned long)g->bar0, nv_size_str(g->bar0_size, s0),
	        (unsigned long)g->bar1, nv_size_str(g->bar1_size, s1), nv_size_str(g->bar1_mapped, s2),
	        g->msi_vec >= 0 ? "MSI vector " : "no MSI ", g->msi_vec,
	        g->chip.open_modules ? "yes (Turing or later)" : "no (needs Turing or later)");
}
