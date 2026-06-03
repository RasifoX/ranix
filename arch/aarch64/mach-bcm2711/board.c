#include <drivers/serial/uart.h>
#include <drivers/irqchip/irqchip.h>
#include <drivers/irqchip/arm_timer.h>
#include <drivers/mailbox/mbox.h>
#include <kernel/board.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <asm/mmu.h>

static board_config_t config = {
	.name          = "Raspberry Pi 4",
	.uart_base     = 0xFE201000UL,
	.gic_dist_base = 0xFF841000UL,
	.gic_cpu_base  = 0xFF842000UL,
	.local_base    = 0xFF800000UL,
	.mbox_base     = 0xFE00B880UL,
	.sd_base       = 0xFE340000UL,
	.bus_offset    = 0xC0000000U,
	.mem_base      = 0x00000000UL,
	.mem_size      = 0x3C000000UL,
	.fb_width      = 800,
	.fb_height     = 480,
	/* BCM2711 armstub parks secondaries in a spin-table, each polling its
	 * release address in low memory (cpu0=0xd8, cpu1=0xe0, cpu2=0xe8,
	 * cpu3=0xf0). Write the entry point there + SEV to start them — the
	 * kernel has taken over EL2, so PSCI-over-hvc is unavailable here. */
	.smp_release_addr = { 0xd8, 0xe0, 0xe8, 0xf0, 0, 0, 0, 0 },
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

	config.uart_base = PHYS_TO_VIRT(config.uart_base);
	uart_init();
}

int board_fb_alloc(fb_info_t *out)
{
	config.mbox_base = PHYS_TO_VIRT(config.mbox_base);
	mbox_init(config.mbox_base, config.bus_offset);

	u32 w = config.fb_width, h = config.fb_height;
	if (mbox_get_display_size(&w, &h) == 0)
		kprintf("display: %dx%d (GPU)\n", (int)w, (int)h);
	else
		kprintf("display: %dx%d (default)\n", (int)w, (int)h);

	if (mbox_fb_init(w, h, out) != 0)
		return -1;
	out->set_virt_offset = mbox_set_virt_offset;
	return 0;
}

void board_irq_init(void)
{
	config.gic_dist_base = PHYS_TO_VIRT(config.gic_dist_base);
	config.gic_cpu_base  = PHYS_TO_VIRT(config.gic_cpu_base);
	config.local_base    = PHYS_TO_VIRT(config.local_base);

	irqchip_init(config.gic_dist_base, config.gic_cpu_base);

	irq_register(27, timer_handler);
	irqchip_enable_irq(27);

	extern void smp_ipi_handler(void);
	irq_register(0, smp_ipi_handler);  /* SGI #0 = reschedule IPI */

	timer_init(TIMER_HZ);
}

#include <drivers/block/sdcard.h>

void board_drivers_register(void)
{
	/* already initialized early */
	pi4_sd_init();
	extern void bcmgenet_init(void);   /* Pi 4 built-in gigabit ethernet */
	bcmgenet_init();
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

#define PM_BASE     PHYS_TO_VIRT(0xFE100000UL)
#define PM_RSTC     (*(volatile uint32_t *)(PM_BASE + 0x1C))
#define PM_WDOG     (*(volatile uint32_t *)(PM_BASE + 0x24))
#define PM_PASSWORD 0x5A000000U

void board_secondary_irq_init(void)
{
	extern void irqchip_secondary_init(uint64_t cpu_base);
	irqchip_secondary_init(config.gic_cpu_base);

	extern void timer_secondary_init(void);
	timer_secondary_init();

	/* Route this core's virtual-timer IRQ to itself via the BCM2711 local
	 * controller (per-core register: local_base + 0x40 + 4*core). */
	uint64_t mpidr;
	__asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
	uint32_t cpu = (uint32_t)(mpidr & 0xff);
	*(volatile uint32_t *)(config.local_base + 0x40 + 4 * cpu) = (1 << 3);
}

void board_reset(void)
{
	PM_WDOG = PM_PASSWORD | 1U;
	PM_RSTC = PM_PASSWORD | 0x00000020U; /* WRCFG_FULL_RESET */
	for (;;) __asm__ volatile("wfe");
}

void board_poweroff(void) { board_reset(); }

/* Pi4 has no PL031; its RTC (if any) is on I2C — not wired yet → no epoch. */
int64_t arch_rtc_read(void) { return 0; }
void    arch_rtc_write(int64_t epoch) { (void)epoch; }

void board_get_reserved(uint64_t *base, uint32_t *size) { *base = 0; *size = 0; }
