#ifndef ASM_CONTEXT_H
#define ASM_CONTEXT_H

#include <uapi/types.h>

/*
 * offsets used by exceptions_asm.S (hardcoded):
 *   0   x0-x30
 *   248 sp
 *   256 elr
 *   264 spsr
 */
typedef struct {
	uint64_t regs[31];
	uint64_t sp;
	uint64_t elr;
	uint64_t spsr;
} arch_context_t;

#define ARCH_SPSR_KERNEL_TASK  0x00000005UL  /* EL1h, DAIF=0 */
#define ARCH_SPSR_USER_TASK    0x00000000UL  /* EL0t, DAIF=0 */

/* syscall register convention (Linux aarch64 ABI) */
#define CTX_SYSCALL_N(ctx)      ((ctx)->regs[8])
#define CTX_SYSCALL_ARG0(ctx)   ((ctx)->regs[0])
#define CTX_SYSCALL_ARG1(ctx)   ((ctx)->regs[1])
#define CTX_SYSCALL_ARG2(ctx)   ((ctx)->regs[2])
#define CTX_SYSCALL_ARG3(ctx)   ((ctx)->regs[3])
#define CTX_SYSCALL_RET(ctx, v) ((ctx)->regs[0] = (uint64_t)(long)(v))

/* Linux aarch64 syscall numbers */
#define SYS_ARCH_READ    63
#define SYS_ARCH_WRITE   64
#define SYS_ARCH_EXIT    93
#define SYS_ARCH_GETPID 172

#endif
