#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <kernel/hal.h>

#define MAX_IRQS 128

static irq_handler_t handlers[MAX_IRQS];

void irq_init(void) {
    for (int i = 0; i < MAX_IRQS; i++)
        handlers[i] = 0;
}

void irq_register(uint32_t irq, irq_handler_t handler) {
    if (irq < MAX_IRQS)
        handlers[irq] = handler;
}

void irq_handle(uint32_t irq) {
    if (irq < MAX_IRQS && handlers[irq]) {
        arch_get_cpu_ctx()->wd_where = "irq_handle";
        handlers[irq]();
    } else
        kprintf("irq: unhandled #%d\n", irq);
}