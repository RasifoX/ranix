/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include "pic8259.h"
#include <asm/io.h>
#include <asm/idt.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <asm/cpu.h>

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1
#define PIC_EOI    0x20

/* remap PIC1 to 0x20-0x27, PIC2 to 0x28-0x2F */
void pic8259_init(void)
{
	/* ICW1: cascade, edge-triggered, ICW4 needed */
	outb(PIC1_CMD,  0x11); io_wait();
	outb(PIC2_CMD,  0x11); io_wait();
	/* ICW2: vector offsets */
	outb(PIC1_DATA, 0x20); io_wait();
	outb(PIC2_DATA, 0x28); io_wait();
	/* ICW3: master has slave on IRQ2; slave ID = 2 */
	outb(PIC1_DATA, 0x04); io_wait();
	outb(PIC2_DATA, 0x02); io_wait();
	/* ICW4: 8086 mode */
	outb(PIC1_DATA, 0x01); io_wait();
	outb(PIC2_DATA, 0x01); io_wait();
	/* mask all except IRQ0 (timer) */
	outb(PIC1_DATA, 0xFE);
	outb(PIC2_DATA, 0xFF);

	kprintf(KERN_INFO "irq: pic 8259\n");
}

void pic8259_eoi(uint32_t irq)
{
	if (irq >= 8)
		outb(PIC2_CMD, PIC_EOI);
	outb(PIC1_CMD, PIC_EOI);
}

void pic8259_mask(uint32_t irq)
{
	uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
	uint8_t bit   = 1 << (irq & 7);
	outb(port, inb(port) | bit);
}

void pic8259_unmask(uint32_t irq)
{
	uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
	uint8_t bit   = 1 << (irq & 7);
	outb(port, inb(port) & ~bit);
}

void irqchip_init(uint64_t a, uint64_t b)
{
	(void)a; (void)b;
	pic8259_init();
}

void irqchip_enable_irq(uint32_t irq)
{
	uint32_t line = irq - 0x20;
	if (line >= 8) pic8259_unmask(2);   /* slave reaches the CPU via cascade IRQ2 */
	pic8259_unmask(line);
}

/* IRQ7 and IRQ15 are what an 8259 reports when a line drops before the CPU's
 * acknowledge. In that case the in-service bit is clear; no EOI is owed for the
 * spurious chip, though a slave spurious still leaves the cascade in service on
 * the master. */
static int pic8259_spurious(uint32_t line)
{
	if (line != 7 && line != 15) return 0;
	uint16_t port = (line == 7) ? PIC1_CMD : PIC2_CMD;
	outb(port, 0x0B);                 /* OCW3: next read returns ISR */
	int in_service = inb(port) & 0x80;
	outb(port, 0x0A);                 /* back to IRR */
	if (in_service) return 0;
	if (line == 15) outb(PIC1_CMD, PIC_EOI);
	return 1;
}

void irqchip_mask_unhandled(uint32_t irq)
{
	if (irq >= 0x20 && irq < 0x30) pic8259_mask(irq - 0x20);
}

void irq_dispatch(uint32_t irq)
{
	if (irq >= VEC_MSI_FIRST) {               /* came through the LAPIC, not the PIC */
		lapic_eoi();
		irq_handle(irq);
		return;
	}
	if (irq >= 0x20 && irq < 0x30 && pic8259_spurious(irq - 0x20)) return;
	pic8259_eoi(irq - 0x20);
	irq_handle(irq);
}
