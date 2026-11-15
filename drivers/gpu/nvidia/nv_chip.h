/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* NVIDIA GPU identification, the pure half of drivers/gpu/nvidia/nv_probe.c:
 * which PCI functions are GPUs, what NV_PMC_BOOT_0 says, what a BAR is.
 * No hardware access here, so tests/nv_host runs it natively.
 *
 * Register facts come from public documentation only (no NVIDIA or Linux code):
 * - NV_PMC_BOOT_0 at BAR0 + 0x0, fields MINOR_REVISION 3:0, MAJOR_REVISION 7:4,
 *   IMPLEMENTATION 23:20, ARCHITECTURE 28:24, and the ARCHITECTURE values
 *   0x0C (GF100) .. 0x17 (GA100): NVIDIA open-gpu-doc (MIT),
 *   manuals/turing/tu104/dev_master.ref.txt and manuals/ampere/ga100/dev_boot.ref.txt.
 *   NV_PMC_BOOT_42 at BAR0 + 0xA00, same ARCHITECTURE/IMPLEMENTATION fields: same files.
 * - Chip names from the chip id (ARCHITECTURE:IMPLEMENTATION, e.g. 0x118 = GM108):
 *   envytools documentation (docs/gpu.rst chipset list). Values above 0x17
 *   (0x18 Hopper, 0x19 Ada, 0x1A/0x1B Blackwell) are not in open-gpu-doc; they are
 *   from public chip lists and are confirmed only when such a GPU is read
 *   (expected: RTX 4050 Laptop, 10de:28e1, AD107 = 0x197).
 * - PCI class codes and BAR layout: PCI Local Bus Specification 3.0, 6.2.5.
 */
#ifndef DRIVERS_GPU_NVIDIA_NV_CHIP_H
#define DRIVERS_GPU_NVIDIA_NV_CHIP_H
#include <uapi/types.h>

#define NV_VENDOR            0x10DE
#define NV_PMC_BOOT_0        0x000
#define NV_PMC_BOOT_42       0xA00
#define NV_ARCH_TURING       0x16   /* NVIDIA's open kernel modules start here (their README) */

typedef struct {
	uint32_t    boot0;
	uint16_t    chip;        /* ARCHITECTURE:IMPLEMENTATION, bits 28:20: 0x118 */
	uint8_t     arch;        /* bits 28:24 */
	uint8_t     impl;        /* bits 23:20 */
	uint8_t     rev_major;   /* bits 7:4, 0xA = "A" */
	uint8_t     rev_minor;   /* bits 3:0 */
	const char *family;      /* "Maxwell", or "unknown" */
	char        name[8];     /* "GM108", or "" when the id is not in the table */
	int         open_modules;/* 1: Turing or later, which the open modules support */
} nv_chip_t;

/* 1 if this function is an NVIDIA display controller: vendor 0x10DE and class 03
 * (subclass 00 VGA, 02 3D, 80 other). id = config dword 0x00, class_rev = 0x08.
 * The HD-audio function of the same card (class 04) is not a GPU. */
int nv_pci_is_gpu(uint32_t id, uint32_t class_rev);

/* Decode NV_PMC_BOOT_0.
 *  0: decoded (name may still be "" for an id outside the table)
 * -1: all ones: the GPU does not answer (powered off, D3cold, link down)
 * -2: no architecture we know (0, or below Fermi): not a value to trust */
int nv_boot0_decode(uint32_t boot0, nv_chip_t *c);

/* A memory BAR from its config dword(s). lo = BAR n, hi = BAR n+1 (used only when
 * lo says 64-bit). Returns 0 and the base, -1 for an I/O BAR. */
int nv_bar_decode(uint32_t lo, uint32_t hi, uint64_t *base, int *is64, int *prefetch);

/* The size from the values read back after writing all ones (PCI 3.0 6.2.5.1):
 * lo_mask and hi_mask are those reads; hi_mask is ignored for a 32-bit BAR.
 * 0 when the BAR is not implemented. */
uint64_t nv_bar_size(uint32_t lo_mask, uint32_t hi_mask, int is64);

/* "16M", "256M", "8G", "4K": for log lines. buf >= 16 bytes. */
const char *nv_size_str(uint64_t bytes, char *buf);

#endif
