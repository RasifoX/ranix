#include <kernel/irq.h>
#include <kernel/pci.h>

void irq_enable(void)  { __asm__ volatile("sti"); }
void irq_disable(void) { __asm__ volatile("cli"); }

/* Route one MSI vector to the calling CPU. MSI needs neither the IOAPIC nor the
 * MADT routing tables, and being edge-triggered it cannot storm like a shared
 * level line. Layout from the PCI spec, address and data from the Intel SDM. */
#define MSI_CTRL       0x02
#define MSI_ADDR_LO    0x04
#define MSI_ADDR_HI    0x08
#define MSI_DATA_32    0x08
#define MSI_DATA_64    0x0C
#define MSI_CTRL_EN    0x0001
#define MSI_CTRL_MME   0x0070
#define MSI_CTRL_64BIT 0x0080
#define MSI_ADDR_BASE  0xFEE00000u

int pci_msi_enable(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t vector)
{
	extern uint32_t lapic_self_id(void);
	uint8_t cap = pci_find_cap(bus, dev, fn, PCI_CAP_MSI);
	if (!cap) return -1;

	uint16_t ctrl = pci_read16(bus, dev, fn, cap + MSI_CTRL);
	pci_write32(bus, dev, fn, cap + MSI_ADDR_LO, MSI_ADDR_BASE | (lapic_self_id() << 12));
	if (ctrl & MSI_CTRL_64BIT) {
		pci_write32(bus, dev, fn, cap + MSI_ADDR_HI, 0);
		pci_write16(bus, dev, fn, cap + MSI_DATA_64, vector);
	} else {
		pci_write16(bus, dev, fn, cap + MSI_DATA_32, vector);
	}
	pci_write16(bus, dev, fn, cap + MSI_CTRL, (ctrl & ~MSI_CTRL_MME) | MSI_CTRL_EN);
	return 0;
}

uint64_t irq_save(void)
{
	uint64_t flags;
	__asm__ volatile("pushfq; pop %0; cli" : "=r"(flags));
	return flags;
}

void irq_restore(uint64_t flags)
{
	__asm__ volatile("push %0; popfq" :: "r"(flags) : "memory");
}
