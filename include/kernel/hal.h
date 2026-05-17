#ifndef _KERNEL_HAL_H
#define _KERNEL_HAL_H

#include <asm/context.h>

void arch_halt(void);
void arch_yield(void);
void arch_cpu_init(void);

/* Initialize a fresh CPU context for a kernel task. */
void arch_context_init(arch_context_t *ctx, void (*entry)(void), uint64_t stack_top);

/* Initialize a CPU context that will drop to unprivileged mode (EL0 / ring3). */
void arch_context_init_user(arch_context_t *ctx, uint64_t entry, uint64_t ustack_top);

/* CPU-specific data area */
typedef struct cpu_context {
	struct cpu_context *self;      /* offset 0 */
	struct process     *current;   /* offset 8 */
	uint8_t            *kstack_top; /* offset 16 - current process kstack */
	uint8_t            *irq_stack;  /* offset 24 - dedicated IRQ stack */
	uint64_t            cpu_id;     /* offset 32 */
	uint64_t            magic;      /* offset 40 */
	uint64_t            scratch;    /* offset 48 - scratch space for entry path */
} cpu_context_t;

/* Get the current CPU's context structure. Implemented in arch/ */
cpu_context_t *arch_get_cpu_ctx(void);

/* Update the kernel stack pointer used on privilege transitions (TSS.RSP0 / SP_EL1). */
void arch_set_user_kstack(uint64_t kstack_top);

/* TLS base register save/restore (FS_BASE on x86_64, TPIDR_EL0 on AArch64) */
struct process;
void arch_save_tls(struct process *p);
void arch_restore_tls(struct process *p);

/* Safe user memory access guards */
void arch_user_access_begin(void);
void arch_user_access_end(void);

/* Safe user memory access */
int  access_ok(const void *addr, size_t n);
long copy_from_user(void *dst, const void *src, size_t n);
long copy_to_user(void *dst, const void *src, size_t n);
long strncpy_from_user(char *dst, const char *src, size_t n);

/* Register access abstractions */
static inline uint32_t reg_read32(uint64_t addr) {
	return *(volatile uint32_t *)addr;
}
static inline void reg_write32(uint64_t addr, uint32_t val) {
	*(volatile uint32_t *)addr = val;
}

#endif
