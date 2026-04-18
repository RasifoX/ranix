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
#define ARCH_SPSR_USER_TASK    0x00000000UL  /* EL0t, I=0 F=0 (all unmasked) */

/* syscall register convention (Linux aarch64 ABI) */
#define CTX_SYSCALL_N(ctx)      ((ctx)->regs[8])
#define CTX_SYSCALL_ARG0(ctx)   ((ctx)->regs[0])
#define CTX_SYSCALL_ARG1(ctx)   ((ctx)->regs[1])
#define CTX_SYSCALL_ARG2(ctx)   ((ctx)->regs[2])
#define CTX_SYSCALL_ARG3(ctx)   ((ctx)->regs[3])
#define CTX_SYSCALL_RET(ctx, v) ((ctx)->regs[0] = (uint64_t)(long)(v))

/* Linux aarch64 syscall numbers */
#define SYS_ARCH_IOCTL        29
#define SYS_ARCH_MKDIRAT      34
#define SYS_ARCH_UNLINKAT     35
#define SYS_ARCH_OPENAT       56
#define SYS_ARCH_CLOSE        57
#define SYS_ARCH_GETDENTS64   61
#define SYS_ARCH_LSEEK        62
#define SYS_ARCH_READ         63
#define SYS_ARCH_WRITE        64
#define SYS_ARCH_NEWFSTATAT   79
#define SYS_ARCH_FSTAT        80
#define SYS_ARCH_EXIT         93
#define SYS_ARCH_GETPID      172
#define SYS_ARCH_BRK         214

#endif
