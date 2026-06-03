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

/* syscall register convention (aarch64 ABI) */
#define CTX_SYSCALL_N(ctx)      ((ctx)->regs[8])
#define CTX_SYSCALL_ARG0(ctx)   ((ctx)->regs[0])
#define CTX_SYSCALL_ARG1(ctx)   ((ctx)->regs[1])
#define CTX_SYSCALL_ARG2(ctx)   ((ctx)->regs[2])
#define CTX_SYSCALL_ARG3(ctx)   ((ctx)->regs[3])
#define CTX_SYSCALL_ARG4(ctx)   ((ctx)->regs[4])
#define CTX_SYSCALL_ARG5(ctx)   ((ctx)->regs[5])
#define CTX_SYSCALL_RET(ctx, v) ((ctx)->regs[0] = (uint64_t)(long)(v))

/* aarch64 syscall numbers */
/* Linux syscall numbers (sorted by number; the dispatch in syscall.c
 * references these names). */
#define SYS_ARCH_SETXATTR          5
#define SYS_ARCH_LSETXATTR         6
#define SYS_ARCH_GETXATTR          8
#define SYS_ARCH_LGETXATTR         9
#define SYS_ARCH_FGETXATTR         10
#define SYS_ARCH_LISTXATTR         11
#define SYS_ARCH_LLISTXATTR        12
#define SYS_ARCH_REMOVEXATTR       14
#define SYS_ARCH_GETCWD            17
#define SYS_ARCH_EVENTFD2          19
#define SYS_ARCH_EPOLL_CREATE1     20
#define SYS_ARCH_EPOLL_CTL         21
#define SYS_ARCH_EPOLL_PWAIT       22
#define SYS_ARCH_DUP               23
#define SYS_ARCH_DUP3              24
#define SYS_ARCH_FCNTL             25
#define SYS_ARCH_INOTIFY_INIT1     26
#define SYS_ARCH_INOTIFY_ADD_WATCH 27
#define SYS_ARCH_INOTIFY_RM_WATCH  28
#define SYS_ARCH_IOCTL             29
#define SYS_ARCH_FLOCK             32
#define SYS_ARCH_MKNODAT           33
#define SYS_ARCH_MKDIRAT           34
#define SYS_ARCH_UNLINKAT          35
#define SYS_ARCH_SYMLINKAT         36
#define SYS_ARCH_LINKAT            37
#define SYS_ARCH_RENAMEAT          38
#define SYS_ARCH_UMOUNT2           39
#define SYS_ARCH_MOUNT             40
#define SYS_ARCH_STATFS            43
#define SYS_ARCH_FSTATFS           44
#define SYS_ARCH_FTRUNCATE         46
#define SYS_ARCH_FALLOCATE         47
#define SYS_ARCH_FACCESSAT         48
#define SYS_ARCH_CHROOT            51
#define SYS_ARCH_PIVOT_ROOT        41
#define SYS_ARCH_CHDIR             49
#define SYS_ARCH_FCHDIR            50
#define SYS_ARCH_FCHMOD            52
#define SYS_ARCH_FCHMODAT          53
#define SYS_ARCH_FCHOWNAT          54
#define SYS_ARCH_FCHOWN            55
#define SYS_ARCH_OPENAT            56
#define SYS_ARCH_CLOSE             57
#define SYS_ARCH_VFORK             58
#define SYS_ARCH_PIPE2             59
#define SYS_ARCH_GETDENTS64        61
#define SYS_ARCH_LSEEK             62
#define SYS_ARCH_READ              63
#define SYS_ARCH_WRITE             64
#define SYS_ARCH_READV             65
#define SYS_ARCH_WRITEV            66
#define SYS_ARCH_PREAD64           67
#define SYS_ARCH_PWRITE64          68
#define SYS_ARCH_SENDFILE          71
#define SYS_ARCH_PSELECT6          72
#define SYS_ARCH_PPOLL             73
#define SYS_ARCH_SIGNALFD4         74
#define SYS_ARCH_SPLICE            76
#define SYS_ARCH_READLINKAT        78
#define SYS_ARCH_NEWFSTATAT        79
#define SYS_ARCH_FSTAT             80
#define SYS_ARCH_SYNC              81
#define SYS_ARCH_FSYNC             82
#define SYS_ARCH_FDATASYNC         83
#define SYS_ARCH_TIMERFD_CREATE    85
#define SYS_ARCH_TIMERFD_SETTIME   86
#define SYS_ARCH_TIMERFD_GETTIME   87
#define SYS_ARCH_UTIMENSAT         88
#define SYS_ARCH_CAPGET            90
#define SYS_ARCH_CAPSET            91
#define SYS_ARCH_EXIT              93
#define SYS_ARCH_EXIT_GROUP        94
#define SYS_ARCH_WAITID            95
#define SYS_ARCH_SET_TID_ADDR      96
#define SYS_ARCH_UNSHARE           97
#define SYS_ARCH_FUTEX             98
#define SYS_ARCH_SET_ROBUST_LIST   99
#define SYS_ARCH_GET_ROBUST_LIST   100
#define SYS_ARCH_NANOSLEEP         101
#define SYS_ARCH_GETITIMER         102
#define SYS_ARCH_SETITIMER         103
#define SYS_ARCH_CLOCK_SETTIME     112
#define SYS_ARCH_CLOCK_GETTIME     113
#define SYS_ARCH_CLOCK_NANOSLEEP   115
#define SYS_ARCH_SYSLOG            116
#define SYS_ARCH_PTRACE            117
#define SYS_ARCH_SCHED_SETAFFINITY 122
#define SYS_ARCH_SCHED_SETPARAM        118
#define SYS_ARCH_SCHED_SETSCHEDULER    119
#define SYS_ARCH_SCHED_GETSCHEDULER    120
#define SYS_ARCH_SCHED_GETPARAM        121
#define SYS_ARCH_SCHED_GET_PRIORITY_MAX 125
#define SYS_ARCH_SCHED_GET_PRIORITY_MIN 126
#define SYS_ARCH_SCHED_GETAFFINITY 123
#define SYS_ARCH_SCHED_YIELD       124
#define SYS_ARCH_KILL              129
#define SYS_ARCH_TKILL             130
#define SYS_ARCH_TGKILL            131
#define SYS_ARCH_SIGALTSTACK       132
#define SYS_ARCH_RT_SIGSUSPEND     133
#define SYS_ARCH_RT_SIGACTION      134
#define SYS_ARCH_RT_SIGPROCMASK    135
#define SYS_ARCH_RT_SIGPENDING     136
#define SYS_ARCH_RT_SIGRETURN      139
#define SYS_ARCH_SETPRIORITY       140
#define SYS_ARCH_GETPRIORITY       141
#define SYS_ARCH_REBOOT            142
#define SYS_ARCH_SETREGID          143
#define SYS_ARCH_SETGID            144
#define SYS_ARCH_SETREUID          145
#define SYS_ARCH_SETUID            146
#define SYS_ARCH_SETRESUID         147
#define SYS_ARCH_GETRESUID         148
#define SYS_ARCH_SETRESGID         149
#define SYS_ARCH_GETRESGID         150
#define SYS_ARCH_TIMES             153
#define SYS_ARCH_SETPGID           154
#define SYS_ARCH_GETPGID           155
#define SYS_ARCH_GETSID            156
#define SYS_ARCH_SETSID            157
#define SYS_ARCH_GETGROUPS         158
#define SYS_ARCH_SETGROUPS         159
#define SYS_ARCH_UNAME             160
#define SYS_ARCH_SETHOSTNAME       161
#define SYS_ARCH_GETRLIMIT         163
#define SYS_ARCH_SETRLIMIT         164
#define SYS_ARCH_GETRUSAGE         165
#define SYS_ARCH_UMASK             166
#define SYS_ARCH_PRCTL             167
#define SYS_ARCH_GETTIMEOFDAY      169
#define SYS_ARCH_GETPID            172
#define SYS_ARCH_GETPPID           173
#define SYS_ARCH_GETUID            174
#define SYS_ARCH_GETEUID           175
#define SYS_ARCH_GETGID            176
#define SYS_ARCH_GETEGID           177
#define SYS_ARCH_GETTID            178
#define SYS_ARCH_SYSINFO           179
#define SYS_ARCH_MQ_OPEN           180
#define SYS_ARCH_MQ_UNLINK         181
#define SYS_ARCH_MQ_TIMEDSEND      182
#define SYS_ARCH_MQ_TIMEDRECEIVE   183
#define SYS_ARCH_MQ_GETSETATTR     185
#define SYS_ARCH_MSGGET            186
#define SYS_ARCH_MSGCTL            187
#define SYS_ARCH_MSGRCV            188
#define SYS_ARCH_MSGSND            189
#define SYS_ARCH_SEMGET            190
#define SYS_ARCH_SEMCTL            191
#define SYS_ARCH_SEMOP             193
#define SYS_ARCH_SHMGET            194
#define SYS_ARCH_SHMCTL            195
#define SYS_ARCH_SHMAT             196
#define SYS_ARCH_SHMDT             197
#define SYS_ARCH_SOCKET            198
#define SYS_ARCH_SOCKETPAIR        199
#define SYS_ARCH_BIND              200
#define SYS_ARCH_LISTEN            201
#define SYS_ARCH_ACCEPT            202
#define SYS_ARCH_CONNECT           203
#define SYS_ARCH_GETSOCKNAME       204
#define SYS_ARCH_GETPEERNAME       205
#define SYS_ARCH_SENDTO            206
#define SYS_ARCH_RECVFROM          207
#define SYS_ARCH_SETSOCKOPT        208
#define SYS_ARCH_GETSOCKOPT        209
#define SYS_ARCH_SHUTDOWN          210
#define SYS_ARCH_SENDMSG           211
#define SYS_ARCH_RECVMSG           212
#define SYS_ARCH_BRK               214
#define SYS_ARCH_MUNMAP            215
#define SYS_ARCH_MREMAP            216
#define SYS_ARCH_CLONE             220
#define SYS_ARCH_EXECVE            221
#define SYS_ARCH_MMAP              222
#define SYS_ARCH_MPROTECT          226
#define SYS_ARCH_MSYNC             227
#define SYS_ARCH_MLOCK             228
#define SYS_ARCH_MUNLOCK           229
#define SYS_ARCH_FADVISE64         223
#define SYS_ARCH_MADVISE           233
#define SYS_ARCH_ACCEPT4           242
#define SYS_ARCH_PROCESS_VM_READV  270
#define SYS_ARCH_PROCESS_VM_WRITEV 271
#define SYS_ARCH_WAIT4             260
#define SYS_ARCH_PRLIMIT64         261
#define SYS_ARCH_SETNS             268
#define SYS_ARCH_RENAMEAT2         276
#define SYS_ARCH_GETRANDOM         278
#define SYS_ARCH_MEMFD_CREATE      279
#define SYS_ARCH_MEMBARRIER        283
#define SYS_ARCH_MLOCK2            284
#define SYS_ARCH_COPY_FILE_RANGE   285
#define SYS_ARCH_STATX             291
#define SYS_ARCH_RSEQ              293
#define SYS_ARCH_FACCESSAT2        439
#define SYS_ARCH_EXECVEAT          281
#define SYS_ARCH_SECCOMP           277
#define SYS_ARCH_PIDFD_SEND_SIGNAL 424
#define SYS_ARCH_PIDFD_OPEN        434
#define SYS_ARCH_CLONE3            435
#define SYS_ARCH_FORK              1079 /* not a real AArch64 syscall; clone handles it */

#endif
