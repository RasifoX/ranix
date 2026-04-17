#include <drivers/serial/pl011.h>
#include <drivers/irqchip/irqchip.h>
#include <drivers/irqchip/arm_timer.h>
#include <kernel/board.h>
#include <kernel/irq.h>

static board_config_t config = {
	.name          = "QEMU virt",
	.uart_base     = 0x09000000UL,
	.gic_dist_base = 0x08000000UL,
	.gic_cpu_base  = 0x08010000UL,
	.mem_base      = 0x40000000UL,
	.mem_size      = 128 * 1024 * 1024,
	.fb_width      = 0,
	.fb_height     = 0,
};

void board_early_init(void)
{
	uart_init();
}

int board_fb_alloc(fb_info_t *out)
{
	(void)out;
	return -1;
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
