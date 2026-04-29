#include <kernel/pci.h>
#include <kernel/kprintf.h>

#define PCI_ADDR 0xCF8
#define PCI_DATA 0xCFC

#if defined(__x86_64__)
#include <asm/io.h>

static uint32_t pci_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
	return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)(dev & 0x1F) << 11)
	     | ((uint32_t)(fn & 0x07) << 8) | (reg & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
	outl(PCI_ADDR, pci_addr(bus, dev, fn, reg));
	return inl(PCI_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg)
{
	outl(PCI_ADDR, pci_addr(bus, dev, fn, reg));
	return (uint16_t)(inl(PCI_DATA) >> ((reg & 2) * 8));
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t v)
{
	outl(PCI_ADDR, pci_addr(bus, dev, fn, reg));
	outl(PCI_DATA, v);
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint16_t v)
{
	outl(PCI_ADDR, pci_addr(bus, dev, fn, reg));
	uint32_t cur = inl(PCI_DATA);
	int shift = (reg & 2) * 8;
	cur = (cur & ~(0xFFFFu << shift)) | ((uint32_t)v << shift);
	outl(PCI_DATA, cur);
}

int pci_find(uint16_t vendor, uint16_t device, uint8_t *rbus, uint8_t *rdev, uint8_t *rfn)
{
	for (int b = 0; b < 256; b++) {
		for (int d = 0; d < 32; d++) {
			uint32_t id = pci_read32((uint8_t)b, (uint8_t)d, 0, PCI_VENDOR_ID);
			if ((id & 0xFFFF) == vendor && (id >> 16) == device) {
				*rbus = (uint8_t)b;
				*rdev = (uint8_t)d;
				*rfn  = 0;
				return 0;
			}
		}
	}
	return -1;
}

#else

uint32_t pci_read32(uint8_t b, uint8_t d, uint8_t f, uint8_t r) { (void)b;(void)d;(void)f;(void)r; return 0xFFFFFFFF; }
uint16_t pci_read16(uint8_t b, uint8_t d, uint8_t f, uint8_t r) { (void)b;(void)d;(void)f;(void)r; return 0xFFFF; }
void pci_write32(uint8_t b, uint8_t d, uint8_t f, uint8_t r, uint32_t v) { (void)b;(void)d;(void)f;(void)r;(void)v; }
void pci_write16(uint8_t b, uint8_t d, uint8_t f, uint8_t r, uint16_t v) { (void)b;(void)d;(void)f;(void)r;(void)v; }
int pci_find(uint16_t v, uint16_t d, uint8_t *rb, uint8_t *rd, uint8_t *rf) { (void)v;(void)d;(void)rb;(void)rd;(void)rf; return -1; }

#endif
