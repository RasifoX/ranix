#ifndef SYSCALL_H
#define SYSCALL_H

#include <asm/context.h>

void syscall_handle(arch_context_t *ctx);

#endif
