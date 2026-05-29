#include <drivers/serial/uart16550/uart16550.h>
#include <drivers/irqchip/pic8259/pic8259.h>
#include <drivers/irqchip/pit/pit.h>
#include <asm/gdt.h>
#include <asm/idt.h>
#include <kernel/board.h>
#include <kernel/irq.h>

static board_config_t config = {
	.name     = "x86 PC",
	.uart_base = 0x3F8,
	.mem_base  = 0,
	.mem_size  = 128UL * 1024 * 1024,
	.fb_width  = 0,
	.fb_height = 0,
};

extern uint64_t _dtb_ptr;
void multiboot2_parse(uint64_t ptr, uint64_t *mem_size_out);

void board_early_init(void)
{
	uart16550_init();
	multiboot2_parse(_dtb_ptr, &config.mem_size);
}

extern int mb2_get_fb(fb_info_t *out);
extern void mb2_foreach_mem_region(void (*cb)(uint64_t, uint64_t, void *), void *ud);
extern void mb2_get_reserved(uint64_t *base, uint32_t *size);

int board_fb_alloc(fb_info_t *out)
{
	return mb2_get_fb(out);
}

#include <drivers/input/ps2.h>

void board_irq_init(void)
{
	gdt_init(0);
	idt_init();
	pic8259_init();
	irq_register(0x20, pit_handler);
	pit_init(TIMER_HZ);
	ps2_kbd_init();
}

void board_secondary_irq_init(void)
{
	/* x86_64: PIT IRQ0 is broadcast via PIC to all CPUs.
	 * Secondary CPUs only need to load GDT/IDT (done in arch_secondary_init). */
}

extern void virtio_net_pci_init(void);

#include <drivers/block/sdcard.h>

void board_drivers_register(void)
{
	uart16550_register();
	pit_register();
	virtio_blk_init();
	virtio_net_pci_init();
}

board_config_t *board_get_config(void) { return &config; }

void board_foreach_mem_region(void (*cb)(uint64_t, uint64_t, void *), void *ud)
{
    mb2_foreach_mem_region(cb, ud);
}

void board_get_reserved(uint64_t *base, uint32_t *size)
{
    mb2_get_reserved(base, size);
}

static uint8_t inb_port(uint16_t port)
{
	uint8_t val;
	__asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
	return val;
}

static void outb_port(uint16_t port, uint8_t val)
{
	__asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

void board_poweroff(void)
{
	/* QEMU isa-debug-exit (port 0x501, any value → QEMU exits with code 2*val+1) */
	__asm__ volatile("outb %0, %1" :: "a"((uint8_t)0x00), "Nd"((uint16_t)0x501));
	/* QEMU ACPI shutdown PIIX4: PM1a_CNT at 0x604, SLP_EN|SLP_TYP_S5 */
	__asm__ volatile("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
	/* Older QEMU: port 0xb004 */
	__asm__ volatile("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0xb004));
	for (;;) __asm__ volatile("hlt");
}

void board_reset(void)
{
	/* Linux-style cf9 hard reset: read-modify-write with delay */
	uint8_t cf9 = inb_port(0xcf9) & ~6;
	outb_port(0xcf9, cf9 | 2);   /* assert RSTIN# */
	for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause");
	outb_port(0xcf9, cf9 | 6);   /* assert RSTIN# + HRST */
	for (volatile int i = 0; i < 100000; i++) __asm__ volatile("pause");
	/* Fallback: keyboard controller */
	outb_port(0x64, 0xFE);
	/* Last resort: triple fault */
	static const uint8_t null_idt[10] = {0};
	__asm__ volatile("cli; lidt %0; int3" :: "m"(null_idt) : "memory");
	for (;;) __asm__ volatile("hlt");
}
