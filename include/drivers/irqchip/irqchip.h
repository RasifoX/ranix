#ifndef IRQCHIP_H
#define IRQCHIP_H
#include <uapi/types.h>

void irqchip_init(uint64_t dist_base, uint64_t cpu_base);
void irqchip_enable_irq(uint32_t irq);

#endif
