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
int pci_find_class(uint8_t cls, uint8_t sub, uint8_t prog, uint8_t *bus, uint8_t *devn, uint8_t *fn);

/* capability list walk (feature discovery) */
int     pci_cap_ids(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t *ids, int max);
uint8_t pci_find_cap(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t cap_id);
#define PCI_CAP_PM    0x01   /* power management */
#define PCI_CAP_MSI   0x05
#define PCI_CAP_VNDR  0x09   /* vendor-specific (virtio modern) */
#define PCI_CAP_PCIE  0x10   /* PCI Express */
#define PCI_CAP_MSIX  0x11

#define PCI_VENDOR_ID   0x00
#define PCI_DEVICE_ID   0x02
#define PCI_COMMAND     0x04
#define PCI_BAR0        0x10
#define PCI_BAR1        0x14
#define PCI_BAR5        0x24
#define PCI_INT_LINE    0x3C   /* IRQ line (low byte), int pin (next byte) */

#define PCI_CMD_IO      0x01
#define PCI_CMD_MEM     0x02
#define PCI_CMD_MASTER  0x04

#endif
