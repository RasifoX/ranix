#ifndef KERNEL_PTRACE_H
#define KERNEL_PTRACE_H

/* Hooks the rest of the kernel calls into ptrace (kernel/sys/syscall_ptrace.c). */

#include <kernel/proc.h>

int  ptrace_trap(int si_code);
void ptrace_apply_singlestep(arch_context_t *ctx);
int  ptrace_signal_stop(arch_context_t *ctx, int sig, sigqueue_t *q);
void ptrace_group_stop(int sig);
void ptrace_follow_fork(process_t *parent, process_t *child);
void ptrace_follow_clone(process_t *parent, process_t *child);
void ptrace_exec_done(process_t *p);

#endif
