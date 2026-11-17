/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* NVIDIA GPU, the probe step: find the
 * GPU on PCI, bring it to D0, map BAR0 and BAR1 uncached, read NV_PMC_BOOT_0, set
 * up an MSI vector. Nothing else: no bus mastering, no GPU interrupt is enabled,
 * no engine is touched, no firmware is loaded. */
#ifndef DRIVERS_GPU_NVIDIA_NV_PROBE_H
#define DRIVERS_GPU_NVIDIA_NV_PROBE_H
#include "nv_chip.h"

enum {
	NV_ST_NONE = 0,   /* no NVIDIA GPU on PCI (or it is powered off so far that config reads fail) */
	NV_ST_NOBAR,      /* BAR0 unassigned or not memory: left alone */
	NV_ST_OFF,        /* BOOT_0 reads all ones: the GPU does not answer (D3cold, link down) */
	NV_ST_BADID,      /* BOOT_0 is not a value we know: left alone */
	NV_ST_UP,         /* identified; MSI vector set up if the function has MSI */
};

#define NV_BAR1_MAP_MAX (16ULL << 20)   /* BAR1 can be gigabytes (resizable BAR): map at most this */

typedef struct {
	int       state;
	uint8_t   bus, dev, fn;
	uint16_t  device;          /* PCI device id */
	uint8_t   pci_class, pci_sub, pci_rev;
	int       pm_before;       /* PCI PM state found at probe: 0..3 (D0..D3hot), -1 no PM capability */
	uint64_t  bar0, bar0_size, bar1, bar1_size, bar1_mapped;
	volatile uint32_t *regs;   /* BAR0, uncached */
	void     *bar1_va;         /* first bar1_mapped bytes of BAR1, uncached; never read here */
	uint32_t  boot42;          /* NV_PMC_BOOT_42, read only on Turing and later; else 0 */
	nv_chip_t chip;
	int       msi_vec;         /* the vector, or -1 */
	volatile uint64_t irqs;    /* MSIs seen; none are expected, the GPU is not asked for any */
} nv_gpu_t;

void nv_probe_init(void);           /* board_drivers_register(); silent when there is no GPU */
const nv_gpu_t *nv_gpu(void);       /* the first GPU's state, for tests and diagnostics */

#endif
