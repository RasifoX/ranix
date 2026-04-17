#ifndef _KERNEL_HAL_H
#define _KERNEL_HAL_H

#include <asm/context.h>

void arch_halt(void);
void arch_yield(void);

/* Initialize a fresh CPU context for a new kernel task. */
void arch_context_init(arch_context_t *ctx, void (*entry)(void), uint64_t stack_top);

#endif
