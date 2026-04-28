#ifndef ASM_CONTEXT_H
#define ASM_CONTEXT_H

#include <uapi/types.h>

/*
 * offsets used by exceptions_asm.S (hardcoded):
 *   0   x0-x30
 *   248 sp
 *   256 elr
 *   264 spsr
 *   272 q0-q31 (32 * 16 bytes = 512 bytes)
 *   784 fpsr (4 bytes)
 *   788 fpcr (4 bytes)
 *   total size: 792 bytes (aligned to 16)
 */
typedef struct {
	uint64_t regs[31];
	uint64_t sp;
	uint64_t elr;
	uint64_t spsr;
	uint64_t qregs[64]; /* 32 * 128-bit registers as 64 * 64-bit */
	uint32_t fpsr;
	uint32_t fpcr;
	uint64_t _padding_16; /* padding to reach 800 bytes (16-byte aligned) */
} arch_context_t;

#define ARCH_SPSR_KERNEL_TASK  0x00000005UL  /* EL1h, DAIF=0 */
#define ARCH_SPSR_USER_TASK    0x00000000UL  /* EL0t, I=0 F=0 (all unmasked) */

/* syscall register convention (Linux aarch64 ABI) */
#define CTX_SYSCALL_N(ctx)      ((ctx)->regs[8])
#define CTX_SYSCALL_ARG0(ctx)   ((ctx)->regs[0])
#define CTX_SYSCALL_ARG1(ctx)   ((ctx)->regs[1])
#define CTX_SYSCALL_ARG2(ctx)   ((ctx)->regs[2])
#define CTX_SYSCALL_ARG3(ctx)   ((ctx)->regs[3])
#define CTX_SYSCALL_ARG4(ctx)   ((ctx)->regs[4])
#define CTX_SYSCALL_ARG5(ctx)   ((ctx)->regs[5])
#define CTX_SYSCALL_RET(ctx, v) ((ctx)->regs[0] = (uint64_t)(long)(v))

/* Linux aarch64 syscall numbers */
#define SYS_ARCH_GETCWD        17
#define SYS_ARCH_EPOLL_CREATE1 20
#define SYS_ARCH_EPOLL_CTL     21
#define SYS_ARCH_EPOLL_PWAIT   22
#define SYS_ARCH_DUP           23
#define SYS_ARCH_DUP3          24
#define SYS_ARCH_FCNTL         25
#define SYS_ARCH_IOCTL         29
#define SYS_ARCH_MKDIRAT       34
#define SYS_ARCH_UNLINKAT      35
#define SYS_ARCH_SYMLINKAT     36
#define SYS_ARCH_LINKAT        37
#define SYS_ARCH_RENAMEAT      38
#define SYS_ARCH_FTRUNCATE     46
#define SYS_ARCH_FACCESSAT     48
#define SYS_ARCH_CHDIR         49
#define SYS_ARCH_FCHMOD        52
#define SYS_ARCH_FCHMODAT      53
#define SYS_ARCH_FCHOWNAT      54
#define SYS_ARCH_FCHOWN        55
#define SYS_ARCH_OPENAT        56
#define SYS_ARCH_CLOSE         57
#define SYS_ARCH_PIPE2         59
#define SYS_ARCH_GETDENTS64    61
#define SYS_ARCH_LSEEK         62
#define SYS_ARCH_READ          63
#define SYS_ARCH_WRITE         64
#define SYS_ARCH_WRITEV        66
#define SYS_ARCH_PSELECT6      72
#define SYS_ARCH_PPOLL         73
#define SYS_ARCH_READLINKAT    78
#define SYS_ARCH_NEWFSTATAT    79
#define SYS_ARCH_FSTAT         80
#define SYS_ARCH_EXIT          93
#define SYS_ARCH_EXIT_GROUP    94
#define SYS_ARCH_SET_TID_ADDR  96
#define SYS_ARCH_NANOSLEEP    101
#define SYS_ARCH_CLOCK_GETTIME 113
#define SYS_ARCH_KILL          129
#define SYS_ARCH_RT_SIGACTION  134
#define SYS_ARCH_RT_SIGPROCMASK 135
#define SYS_ARCH_SETUID        146
#define SYS_ARCH_SETGID        144
#define SYS_ARCH_TIMES         153
#define SYS_ARCH_SETPGID       154
#define SYS_ARCH_GETPGID       155
#define SYS_ARCH_SETSID        157
#define SYS_ARCH_UNAME         160
#define SYS_ARCH_UMASK         166
#define SYS_ARCH_GETTIMEOFDAY  169
#define SYS_ARCH_GETPID        172
#define SYS_ARCH_GETPPID       173
#define SYS_ARCH_GETUID        174
#define SYS_ARCH_GETEUID       175
#define SYS_ARCH_GETGID        176
#define SYS_ARCH_GETEGID       177
#define SYS_ARCH_SYSINFO       179
#define SYS_ARCH_CLONE         220
#define SYS_ARCH_VFORK         190
#define SYS_ARCH_EXECVE        221
#define SYS_ARCH_BRK           214
#define SYS_ARCH_MUNMAP        215
#define SYS_ARCH_MMAP          222
#define SYS_ARCH_MPROTECT      226
#define SYS_ARCH_WAIT4         260
#define SYS_ARCH_PRLIMIT64     261
#define SYS_ARCH_RENAMEAT2     276

#endif
