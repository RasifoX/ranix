#ifndef SYSCALL_H
#define SYSCALL_H

#include <asm/context.h>

void syscall_handle(arch_context_t *ctx);
long kernel_execve(const char *path, char * const argv[], char * const envp[]);

/* slow-syscall tracer (syscall.c) */
extern int slowsys_on;
uint64_t slowsys_ns(void);

#endif
