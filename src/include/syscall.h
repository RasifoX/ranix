#ifndef SYSCALL_H
#define SYSCALL_H

#include "idt.h"

void syscall_init(void);
void syscall_handler(registers_t *regs);

#endif