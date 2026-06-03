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
	extern void smp_ipi_handler(void);
	irq_register(0, smp_ipi_handler);  /* SGI #0 = reschedule IPI */
}

void board_secondary_irq_init(void)
{
	/* Initialize GIC CPU interface on secondary CPU */
	extern void irqchip_secondary_init(uint64_t cpu_base);
	irqchip_secondary_init(config.gic_cpu_base);
	/* Initialize per-CPU timer */
	extern void timer_secondary_init(void);
	timer_secondary_init();
}

#include <drivers/block/sdcard.h>

void virtio_net_init(void);

void board_drivers_register(void)
{
	pl011_register();
	timer_register();
	virtio_blk_init();
	virtio_net_init();
}

board_config_t *board_get_config(void) { return &config; }

#include <kernel/fdt.h>

struct _fmr { void (*cb)(uint64_t, uint64_t, void *); void *ud; int n; };
static void _fmr_wrap(uint64_t b, uint64_t s, void *ud)
{ struct _fmr *c = ud; c->n++; c->cb(b, s, c->ud); }

void board_foreach_mem_region(void (*cb)(uint64_t, uint64_t, void *), void *ud)
{
    struct _fmr ctx = { cb, ud, 0 };
    fdt_foreach_mem_region(_fmr_wrap, &ctx);
    if (!ctx.n && config.mem_size > 0)
        cb(config.mem_base, config.mem_size, ud);
}


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

void board_poweroff(void)
{
	/* PSCI SYSTEM_OFF (0x84000008) */
	__asm__ volatile(
		"movz x0, #0x8400, lsl #16\n"
		"movk x0, #0x8\n"
		"hvc #0\n"
		::: "x0"
	);
	for (;;) __asm__ volatile("wfe");
}

void board_get_reserved(uint64_t *base, uint32_t *size) { *base = 0; *size = 0; }

/* PL031 RTC @ 0x09010000 (QEMU virt): DR register holds Unix epoch seconds. */
int64_t arch_rtc_read(void)
{
	volatile uint32_t *rtc = (volatile uint32_t *)MMIO_TO_VIRT(0x09010000UL);
	return (int64_t)(uint32_t)rtc[0];
}
