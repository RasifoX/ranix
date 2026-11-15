/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Kernel interfaces drivers/gpu/nvidia/nv_probe.c uses, as the host model in
 * tests/nv_host/model.c provides them. */
#ifndef NV_MODEL_H
#define NV_MODEL_H
#include <stdint.h>
#include <stddef.h>

#define KERN_ERR  ""
#define KERN_WARN ""
#define KERN_INFO ""
void kprintf(const char *fmt, ...);

#define PCI_VENDOR_ID   0x00
#define PCI_COMMAND     0x04
#define PCI_BAR0        0x10
#define PCI_BAR1        0x14
#define PCI_CMD_IO      0x01
#define PCI_CMD_MEM     0x02
#define PCI_CMD_MASTER  0x04
#define PCI_CAP_PM      0x01
#define PCI_CAP_MSI     0x05
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t v);
void     pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint16_t v);
uint8_t  pci_find_cap(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t cap_id);

typedef void (*irq_handler_t)(void);
int pci_msi_attach(uint8_t bus, uint8_t dev, uint8_t fn, irq_handler_t h);

#define VM_READ    (1 << 0)
#define VM_WRITE   (1 << 1)
#define VM_NOCACHE (1 << 4)
#define VM_DEVICE  (1 << 5)
#define VM_WC      (1 << 12)
void *ioremap(uint64_t phys, size_t size, uint32_t flags);

uint8_t  inb(uint16_t port);
uint32_t mmio_read32(volatile uint32_t *reg);

typedef struct { char *p; size_t rem; } hw_out_t;
typedef void (*hwdiag_fn)(hw_out_t *o);
void hw_begin(hw_out_t *o, const char *cls, const char *name);
void hw_kv(hw_out_t *o, const char *key, uint64_t v);
void hw_ks(hw_out_t *o, const char *key, const char *s);
void hw_end(hw_out_t *o);
void hwdiag_register(hwdiag_fn fn);
#endif
