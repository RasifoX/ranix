#include <drivers/serial/pl011.h>
#include <drivers/irqchip/irqchip.h>
#include <drivers/irqchip/arm_timer.h>
#include <drivers/serial/uart.h>
#include <kernel/board.h>
#include <kernel/irq.h>
#include <kernel/mm/vm.h>
#include <asm/mmu.h>

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
	/* enable FP/SIMD at EL0 and EL1 */
	__asm__ volatile(
		"mrs x0, cpacr_el1\n"
		"orr x0, x0, #(3 << 20)\n"
		"msr cpacr_el1, x0\n"
		"isb\n"
		::: "x0"
	);

	config.uart_base = PHYS_TO_VIRT(config.uart_base);
	config.local_base = PHYS_TO_VIRT(config.local_base);
	uart_init();
}

int board_fb_alloc(fb_info_t *out)
{
	(void)out;
	return -1;
}

void board_irq_init(void)
{
	config.gic_dist_base = (uint64_t)ioremap(config.gic_dist_base, 0x1000, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
	config.gic_cpu_base  = (uint64_t)ioremap(config.gic_cpu_base,  0x2000, VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
	irqchip_init(config.gic_dist_base, config.gic_cpu_base);
	irq_register(27, timer_handler);
	irqchip_enable_irq(27);
	timer_init(TIMER_HZ);
}

#include <drivers/block/sdcard.h>

void board_drivers_register(void)
{
	pl011_register();
	timer_register();
	virtio_blk_init();
}

board_config_t *board_get_config(void) { return &config; }


void board_reset(void)
{
	/* PSCI SYSTEM_RESET (0x84000009) */
	__asm__ volatile(
		"movz x0, #0x8400, lsl #16\n"
		"movk x0, #0x9\n"
		"hvc #0\n"
		::: "x0"
	);
	for (;;) __asm__ volatile("wfe");
}
