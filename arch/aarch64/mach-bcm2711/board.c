#include <drivers/serial/pl011.h>
#include <drivers/irqchip/irqchip.h>
#include <drivers/irqchip/arm_timer.h>
#include <drivers/mailbox/mbox.h>
#include <kernel/board.h>
#include <kernel/irq.h>

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
	uart_init();
}

int board_fb_alloc(fb_info_t *out)
{
	mbox_init(config.mbox_base, config.bus_offset);
	if (mbox_fb_init(config.fb_width, config.fb_height, out) != 0)
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
