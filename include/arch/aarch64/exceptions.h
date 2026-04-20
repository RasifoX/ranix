#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H
#include <uapi/types.h>
#include <asm/context.h>

void exceptions_init(void);
void sync_dispatch(arch_context_t *tf);

#endif
