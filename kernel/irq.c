/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <kernel/hal.h>

#define MAX_IRQS 128

/* Per-CPU so the hot path needs no atomics; further CPUs share the last column. */
#define IRQ_STAT_CPUS 32

static irq_handler_t handlers[MAX_IRQS];
static uint64_t      counts[MAX_IRQS][IRQ_STAT_CPUS];

void irq_init(void) {
    for (int i = 0; i < MAX_IRQS; i++)
        handlers[i] = 0;
}

void irq_register(uint32_t irq, irq_handler_t handler) {
    if (irq < MAX_IRQS)
        handlers[irq] = handler;
}

/* A level-triggered line with no handler refires forever and starves the machine,
 * ticks included. Interrupt controllers that can mask a line override this. */
__attribute__((weak)) void irqchip_mask_unhandled(uint32_t irq) { (void)irq; }

/* architectures without MSI routing */
__attribute__((weak)) int pci_msi_attach(uint8_t bus, uint8_t dev, uint8_t fn, irq_handler_t h)
{
    (void)bus; (void)dev; (void)fn; (void)h;
    return -1;
}

void irq_handle(uint32_t irq) {
    if (irq < MAX_IRQS) {
        uint64_t cpu = arch_get_cpu_ctx()->cpu_id;
        counts[irq][cpu < IRQ_STAT_CPUS ? cpu : IRQ_STAT_CPUS - 1]++;
    }
    if (irq < MAX_IRQS && handlers[irq]) {
        arch_get_cpu_ctx()->wd_where = "irq_handle";
        handlers[irq]();
    } else {
        static unsigned reported;
        if (reported < 8) { reported++; kprintf(KERN_WARN "irq: unhandled #%d, masking\n", irq); }
        irqchip_mask_unhandled(irq);
    }
}

/* /proc/interrupts: 0 past the last line; *handler is 0 for an unclaimed one */
uint64_t irq_stat(uint32_t irq, int cpu, uint64_t *handler)
{
    if (irq >= MAX_IRQS || cpu < 0 || cpu >= IRQ_STAT_CPUS) return 0;
    *handler = (uint64_t)handlers[irq];
    return counts[irq][cpu];
}
