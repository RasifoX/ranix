/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/irq.h>
#include <kernel/pci.h>
#include <kernel/sync.h>
#include <asm/idt.h>
#include <asm/cpu.h>

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

/* The vectors above VEC_MSI_FIRST go to whichever device asks first; the handler
 * is registered before the device may send, so its first message is not lost. */
int pci_msi_attach(uint8_t bus, uint8_t dev, uint8_t fn, irq_handler_t h)
{
	static spinlock_t lock = SPINLOCK_INIT;
	int vec = -1;
	uint64_t f = spin_lock_irq(&lock);
	for (uint32_t v = VEC_MSI_FIRST + 1; v <= VEC_MSI_LAST && vec < 0; v++) {
		uint64_t owner = 0;
		irq_stat(v, 0, &owner);
		if (!owner) vec = (int)v;
	}
	if (vec >= 0) irq_register((uint32_t)vec, h);
	spin_unlock_irq(&lock, f);
	if (vec >= 0 && pci_msi_enable(bus, dev, fn, (uint8_t)vec) != 0) {
		irq_register((uint32_t)vec, 0);
		vec = -1;
	}
	return vec;
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
