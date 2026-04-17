#ifndef ASM_IDT_H
#define ASM_IDT_H

#include <uapi/types.h>

void idt_init(void);
void idt_set_gate(uint8_t vec, void *handler, uint8_t flags);

/* exception/irq vector assignments */
#define VEC_TIMER   0x20   /* PIT IRQ0, remapped to 0x20 by PIC */
#define VEC_YIELD   0x40   /* software interrupt for arch_yield() */

#endif
