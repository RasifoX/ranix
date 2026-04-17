#ifndef _KERNEL_HAL_H
#define _KERNEL_HAL_H

#include <asm/context.h>

void arch_halt(void);
void arch_yield(void);

/* Initialize a fresh CPU context for a kernel task. */
void arch_context_init(arch_context_t *ctx, void (*entry)(void), uint64_t stack_top);

/* Initialize a CPU context that will drop to unprivileged mode (EL0 / ring3). */
void arch_context_init_user(arch_context_t *ctx, uint64_t entry, uint64_t ustack_top);

/* Update the kernel stack pointer used on privilege transitions (TSS.RSP0 / SP_EL1). */
void arch_set_user_kstack(uint64_t kstack_top);

#endif
