#ifndef IRQ_H
#define IRQ_H
#include <uapi/types.h>

typedef void (*irq_handler_t)(void);

void irq_init(void);
void irq_register(uint32_t irq, irq_handler_t handler);
void irq_handle(uint32_t irq);
void irq_dispatch(uint32_t irq);
void irq_enable(void);
void irq_disable(void);
uint64_t irq_save(void);
void irq_restore(uint64_t flags);

#endif