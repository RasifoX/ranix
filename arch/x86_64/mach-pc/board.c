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
	multiboot2_parse(_dtb_ptr, &config.mem_size);
	uart16550_init();
}

int board_fb_alloc(fb_info_t *out)
{
	(void)out;
	return -1;
}

#include <drivers/input/ps2.h>

void board_irq_init(void)
{
	gdt_init();
	idt_init();
	pic8259_init();
	irq_register(0x20, pit_handler);
	pit_init(100);
	ps2_kbd_init();
}

#include <drivers/block/sdcard.h>

void board_drivers_register(void)
{
	uart16550_register();
	pit_register();
	virtio_blk_init();
}

board_config_t *board_get_config(void) { return &config; }
