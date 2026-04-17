#include "pic8259.h"
#include <asm/io.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>

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

	kprintf("[PIC] Initialized\n");
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
	pic8259_unmask(irq - 0x20);
}

void irq_dispatch(uint32_t irq)
{
	pic8259_eoi(irq - 0x20);
	irq_handle(irq);
}
