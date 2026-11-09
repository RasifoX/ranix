/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Hardware model interface for the iTCO host test; see tests/itco_host/model.c. */
#ifndef ITCO_MODEL_H
#define ITCO_MODEL_H
#include <stdint.h>
#include <stddef.h>
#define PCI_VENDOR_ID 0x00
#define VM_READ 1
#define VM_WRITE 2
#define VM_NOCACHE 16
#define VM_DEVICE 32
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
void     pci_write8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint8_t v);
uint16_t inw(uint16_t port);
void     outw(uint16_t port, uint16_t val);
uint32_t inl(uint16_t port);
uint32_t mmio_read32(volatile uint32_t *reg);
void     mmio_write32(volatile uint32_t *reg, uint32_t val);
void    *ioremap(uint64_t phys, size_t size, uint32_t flags);
void     kprintf(const char *fmt, ...);
#endif
