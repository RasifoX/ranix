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
#define CTX_SYSCALL_ARG4(ctx)   ((ctx)->r8)
#define CTX_SYSCALL_ARG5(ctx)   ((ctx)->r9)
#define CTX_SYSCALL_RET(ctx, v) ((ctx)->rax = (uint64_t)(long)(v))

/* Linux x86_64 syscall numbers */
#define SYS_ARCH_READ           0
#define SYS_ARCH_WRITE          1
#define SYS_ARCH_OPEN           2
#define SYS_ARCH_CLOSE          3
#define SYS_ARCH_STAT           4
#define SYS_ARCH_FSTAT          5
#define SYS_ARCH_LSTAT          6
#define SYS_ARCH_POLL           7
#define SYS_ARCH_LSEEK          8
#define SYS_ARCH_MMAP           9
#define SYS_ARCH_MPROTECT      10
#define SYS_ARCH_MUNMAP        11
#define SYS_ARCH_BRK           12
#define SYS_ARCH_RT_SIGACTION  13
#define SYS_ARCH_RT_SIGPROCMASK 14
#define SYS_ARCH_IOCTL         16
#define SYS_ARCH_READV         19
#define SYS_ARCH_WRITEV        20
#define SYS_ARCH_ACCESS        21
#define SYS_ARCH_SELECT        23
#define SYS_ARCH_DUP           32
#define SYS_ARCH_DUP2          33
#define SYS_ARCH_NANOSLEEP     35
#define SYS_ARCH_GETPID        39
#define SYS_ARCH_KILL          62
#define SYS_ARCH_UNAME         63
#define SYS_ARCH_TRUNCATE      76
#define SYS_ARCH_FTRUNCATE     77
#define SYS_ARCH_GETCWD        79
#define SYS_ARCH_CHDIR         80
#define SYS_ARCH_RENAME        82
#define SYS_ARCH_MKDIR         83
#define SYS_ARCH_RMDIR         84
#define SYS_ARCH_LINK          86
#define SYS_ARCH_UNLINK        87
#define SYS_ARCH_SYMLINK       88
#define SYS_ARCH_READLINK      89
#define SYS_ARCH_CHMOD         90
#define SYS_ARCH_CHOWN         92
#define SYS_ARCH_UMASK         95
#define SYS_ARCH_GETTIMEOFDAY  96
#define SYS_ARCH_SYSINFO       99
#define SYS_ARCH_TIMES        100
#define SYS_ARCH_GETUID       102
#define SYS_ARCH_SETUID       105
#define SYS_ARCH_GETGID       104
#define SYS_ARCH_SETGID       106
#define SYS_ARCH_GETEUID      107
#define SYS_ARCH_GETEGID      108
#define SYS_ARCH_SETPGID      109
#define SYS_ARCH_GETPPID      110
#define SYS_ARCH_GETPGRP      111
#define SYS_ARCH_SETSID       112
#define SYS_ARCH_GETSID       124
#define SYS_ARCH_GETPRIORITY  140
#define SYS_ARCH_SETPRIORITY  141
#define SYS_ARCH_SIGALTSTACK  131
#define SYS_ARCH_FCNTL         72
#define SYS_ARCH_VFORK         58
#define SYS_ARCH_CLONE         56
#define SYS_ARCH_FORK          57
#define SYS_ARCH_EXECVE        59
#define SYS_ARCH_WAIT4         61
#define SYS_ARCH_EXIT          60
#define SYS_ARCH_EXIT_GROUP   231
#define SYS_ARCH_SET_TID_ADDR 218
#define SYS_ARCH_CLOCK_GETTIME 228
#define SYS_ARCH_PIPE          22
#define SYS_ARCH_PIPE2        293
#define SYS_ARCH_DUP3         292
#define SYS_ARCH_OPENAT       257
#define SYS_ARCH_MKDIRAT      258
#define SYS_ARCH_NEWFSTATAT   262
#define SYS_ARCH_UNLINKAT     263
#define SYS_ARCH_RENAMEAT     264
#define SYS_ARCH_GETDENTS64   217
#define SYS_ARCH_FCHMOD        91
#define SYS_ARCH_FCHMODAT     268
#define SYS_ARCH_FCHOWN        93
#define SYS_ARCH_FCHOWNAT     260
#define SYS_ARCH_READLINKAT   267
#define SYS_ARCH_SYMLINKAT    266
#define SYS_ARCH_LINKAT       265
#define SYS_ARCH_FACCESSAT    269
#define SYS_ARCH_PSELECT6     270
#define SYS_ARCH_FACCESSAT2   439
#define SYS_ARCH_PPOLL        271
#define SYS_ARCH_GETPGID      121
#define SYS_ARCH_ARCH_PRCTL   158
#define SYS_ARCH_PRLIMIT64    302
#define SYS_ARCH_FUTEX        202
#define SYS_ARCH_TGKILL       234
#define SYS_ARCH_SET_ROBUST_LIST 273
#define SYS_ARCH_GET_ROBUST_LIST 274
#define SYS_ARCH_RT_SIGRETURN    15
#define SYS_ARCH_EPOLL_CREATE    213
#define SYS_ARCH_EPOLL_CTL       233
#define SYS_ARCH_EPOLL_WAIT      232
#define SYS_ARCH_EPOLL_CREATE1   291
#define SYS_ARCH_EPOLL_PWAIT     281
#define SYS_ARCH_MSYNC            26
#define SYS_ARCH_MADVISE          28
#define SYS_ARCH_SCHED_YIELD      24
#define SYS_ARCH_SCHED_SETAFFINITY 203
#define SYS_ARCH_SCHED_GETAFFINITY 204
#define SYS_ARCH_PRCTL           157
#define SYS_ARCH_GETRANDOM       318
#define SYS_ARCH_STATX           332
#define SYS_ARCH_MEMBARRIER      324
#define SYS_ARCH_RSEQ            334
#define SYS_ARCH_REBOOT          169
#define SYS_ARCH_SYSLOG          103
#define SYS_ARCH_UTIMENSAT       280
#define SYS_ARCH_FUTIMENS        280  /* same number, used via fd path */
#define SYS_ARCH_GETTID          186
#define SYS_ARCH_GETGROUPS       115
#define SYS_ARCH_SETGROUPS       116
#define SYS_ARCH_GETRLIMIT        97
#define SYS_ARCH_SETRLIMIT       160
#define SYS_ARCH_GETITIMER        36
#define SYS_ARCH_SETITIMER        38
#define SYS_ARCH_SOCKET           41
#define SYS_ARCH_BIND             49
#define SYS_ARCH_CONNECT          42
#define SYS_ARCH_LISTEN           50
#define SYS_ARCH_ACCEPT4         288
#define SYS_ARCH_SENDTO           44
#define SYS_ARCH_RECVFROM         45
#define SYS_ARCH_GETSOCKNAME      51
#define SYS_ARCH_GETPEERNAME      52
#define SYS_ARCH_SETSOCKOPT       54
#define SYS_ARCH_GETSOCKOPT       55
#define SYS_ARCH_SHUTDOWN         48
#define SYS_ARCH_SENDMSG          46
#define SYS_ARCH_RECVMSG          47

#endif
