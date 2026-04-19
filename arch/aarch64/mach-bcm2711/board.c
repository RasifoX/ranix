#include <drivers/serial/pl011.h>
#include <drivers/irqchip/irqchip.h>
#include <drivers/irqchip/arm_timer.h>
#include <drivers/mailbox/mbox.h>
#include <kernel/board.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>

static board_config_t config = {
	.name          = "Raspberry Pi 4",
	.uart_base     = 0xFE201000UL,
	.gic_dist_base = 0xFF841000UL,
	.gic_cpu_base  = 0xFF842000UL,
	.local_base    = 0xFF800000UL,
	.mbox_base     = 0xFE00B880UL,
	.bus_offset    = 0xC0000000U,
	.mem_base      = 0x00000000UL,
	.mem_size      = 0x3C000000UL,
	.fb_width      = 800,
	.fb_height     = 480,
};

void board_early_init(void)
{
	/* enable FP/SIMD at EL0 and EL1 (CPACR_EL1.FPEN = 0b11) */
	__asm__ volatile(
		"mrs x0, cpacr_el1\n"
		"orr x0, x0, #(3 << 20)\n"
		"msr cpacr_el1, x0\n"
		"isb\n"
		::: "x0"
	);
	uart_init();
}

int board_fb_alloc(fb_info_t *out)
{
	mbox_init(config.mbox_base, config.bus_offset);

	u32 w = config.fb_width, h = config.fb_height;
	if (mbox_get_display_size(&w, &h) == 0)
		kprintf("[  mem] display: %dx%d (GPU)\n", (int)w, (int)h);
	else
		kprintf("[  mem] display: %dx%d (default)\n", (int)w, (int)h);

	if (mbox_fb_init(w, h, out) != 0)
		return -1;
	out->set_virt_offset = mbox_set_virt_offset;
	return 0;
}

void board_irq_init(void)
{
	irqchip_init(config.gic_dist_base, config.gic_cpu_base);
	irq_register(27, timer_handler);
	irqchip_enable_irq(27);
	timer_init(100);
}

void board_drivers_register(void)
{
	pl011_register();
	timer_register();
}

board_config_t *board_get_config(void) { return &config; }
