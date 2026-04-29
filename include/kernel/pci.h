#ifndef PCI_H
#define PCI_H

#include <uapi/types.h>

/* PCI config space access (x86_64 port I/O) */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t v);
void     pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint16_t v);

// Returns 0 on success, fills *bus/*dev/*fn
int pci_find(uint16_t vendor, uint16_t device, uint8_t *bus, uint8_t *devn, uint8_t *fn);

#define PCI_VENDOR_ID   0x00
#define PCI_DEVICE_ID   0x02
#define PCI_COMMAND     0x04
#define PCI_BAR0        0x10
#define PCI_BAR1        0x14

#define PCI_CMD_IO      0x01
#define PCI_CMD_MEM     0x02
#define PCI_CMD_MASTER  0x04

#endif
