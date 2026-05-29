#include <drivers/irqchip/irqchip.h>
#include <arch/aarch64/exceptions.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <kernel/board.h>

static uint64_t gicd_base;
static uint64_t gicc_base;
static uint64_t local_irq_src0_addr;

#define GICD(off) (*(volatile uint32_t *)(gicd_base + (off)))
#define GICC(off) (*(volatile uint32_t *)(gicc_base + (off)))

void irqchip_init(uint64_t dist_base, uint64_t cpu_base) {
    gicd_base = dist_base;
    gicc_base = cpu_base;
    local_irq_src0_addr = board_get_config()->local_base + 0x60;

    exceptions_init();

    GICD(0x000) = 0;

    for (int i = 1; i < 4; i++)
        GICD(0x180 + i * 4) = 0xFFFFFFFF;

    GICD(0x180) = 0xFFFF0000;
    GICD(0x280) = 0xFFFFFFFF;

    for (int i = 0; i < 32; i++)
        GICD(0x400 + i * 4) = 0xA0A0A0A0;

    for (int i = 8; i < 32; i++)
        GICD(0x800 + i * 4) = 0x01010101;

    GICD(0x000) = 1;

    GICC(0x004) = 0xF0;
    GICC(0x000) = 1;

    kprintf("GIC: v2\n");
}

void irqchip_enable_irq(uint32_t irq) {
    uint32_t reg = 0x400 + (irq / 4) * 4;
    uint32_t shift = (irq % 4) * 8;
    uint32_t val = GICD(reg);
    val &= ~(0xFF << shift);
    val |= (0x80 << shift);
    GICD(reg) = val;
    GICD(0x100 + (irq / 32) * 4) |= (1u << (irq % 32));
}

void irq_dispatch(uint32_t hint) {
    (void)hint;

    /* Try GIC first — covers PPIs (e.g. virtual timer on virt) and SPIs */
    uint32_t iar = GICC(0x00C);
    uint32_t irq = iar & 0x3FF;
    if (irq < 1020) {
        irq_handle(irq);
        GICC(0x010) = iar;
        return;
    }

    /* BCM2711: per-core timer IRQs come via local controller, not GIC.
     * Each core has its own IRQ-source register (local_base + 0x60 + 4*core). */
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint32_t cpu = (uint32_t)(mpidr & 0xff);
    uint32_t src = *(volatile uint32_t *)(local_irq_src0_addr + 4 * cpu);
    if (src & (1 << 3))
        irq_handle(27);
}

void irqchip_secondary_init(uint64_t cpu_base) {
    volatile uint32_t *cc = (volatile uint32_t *)cpu_base;
    cc[0x004 / 4] = 0xF0;  /* GICC_PMR */
    cc[0x000 / 4] = 1;      /* GICC_CTRL: enable */
    /* Enable PPI 27 (virtual timer) and SGI 0 — banked per-CPU in ISENABLER0 */
    GICD(0x100) |= (1u << 27) | (1u << 0);
}
