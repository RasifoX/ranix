/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <asm/context.h>

void syscall_handle(arch_context_t *ctx);
/* The EINTR this syscall is about to return goes to userspace as it is, even
 * when no signal handler runs: the caller asked to be stopped (KVM_RUN with
 * immediate_exit), so running the call again would never return. */
void syscall_eintr_final(void);
long kernel_execve(const char *path, char * const argv[], char * const envp[]);

/* slow-syscall tracer (syscall.c) */
extern int slowsys_on;
uint64_t slowsys_ns(void);

#endif
