#ifndef ASM_CONTEXT_H
#define ASM_CONTEXT_H

#include <uapi/types.h>

/*
 * Saved on the stack by exceptions_asm.S before calling C handlers.
 * Hardware (CPU) pushes ss/rsp/rflags/cs/rip automatically.
 * We push the rest plus a vector number and error code slot.
 *
 *   offset 0   : fxsave area (512 bytes, 16-byte aligned)
 *   offset 512 : r15-r8, rbp, rdi, rsi, rdx, rcx, rbx, rax  (15 * 8)
 *   offset 632 : vector
 *   offset 640 : error_code
 *   offset 648 : rip, cs, rflags, rsp, ss  (CPU-pushed, 5 * 8)
 *   total      : 688 bytes
 */
typedef struct {
	uint8_t  fxsave[512] __attribute__((aligned(16)));
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
	uint64_t vector;
	uint64_t error_code;
	uint64_t rip, cs, rflags, rsp, ss;
} arch_context_t;

#define ARCH_SPSR_KERNEL_TASK  0x202UL   /* RFLAGS kernel: IF=1 */
#define ARCH_SPSR_USER_TASK    0x202UL   /* RFLAGS user:   IF=1 */

#define GDT_USER_CODE  0x1BUL   /* 0x18 | RPL3 */
#define GDT_USER_DATA  0x23UL   /* 0x20 | RPL3 */

/* syscall register convention (Linux x86_64 ABI) */
#define CTX_SYSCALL_N(ctx)      ((ctx)->rax)
#define CTX_SYSCALL_ARG0(ctx)   ((ctx)->rdi)
#define CTX_SYSCALL_ARG1(ctx)   ((ctx)->rsi)
#define CTX_SYSCALL_ARG2(ctx)   ((ctx)->rdx)
#define CTX_SYSCALL_ARG3(ctx)   ((ctx)->r10)
#define CTX_SYSCALL_RET(ctx, v) ((ctx)->rax = (uint64_t)(long)(v))

/* Linux x86_64 syscall numbers */
#define SYS_ARCH_READ          0
#define SYS_ARCH_WRITE         1
#define SYS_ARCH_WRITEV       20
#define SYS_ARCH_OPEN          2
#define SYS_ARCH_CLOSE         3
#define SYS_ARCH_FSTAT         5
#define SYS_ARCH_LSEEK         8
#define SYS_ARCH_BRK          12
#define SYS_ARCH_IOCTL        16
#define SYS_ARCH_GETPID       39
#define SYS_ARCH_EXIT         60
#define SYS_ARCH_EXIT_GROUP   231
#define SYS_ARCH_SET_TID_ADDR 218
#define SYS_ARCH_MMAP           9
#define SYS_ARCH_MUNMAP        11
#define SYS_ARCH_RT_SIGACTION   13
#define SYS_ARCH_RT_SIGPROCMASK 14
#define SYS_ARCH_VFORK          58
#define SYS_ARCH_CLONE          56
#define SYS_ARCH_EXECVE         59
#define SYS_ARCH_WAIT4          61
#define SYS_ARCH_MKDIR        83
#define SYS_ARCH_UNLINK       87
#define SYS_ARCH_GETDENTS64  217
#define SYS_ARCH_OPENAT      257
#define SYS_ARCH_NEWFSTATAT  262
#define SYS_ARCH_UNLINKAT    263
#define SYS_ARCH_MKDIRAT     258
#define SYS_ARCH_ARCH_PRCTL  158
#define SYS_ARCH_MPROTECT     10
#define SYS_ARCH_PRLIMIT64   302

#endif
