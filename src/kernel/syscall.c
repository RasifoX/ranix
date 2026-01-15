#include <stdint.h>
#include "idt.h"
#include "syscall.h"
#include "stdio.h"

void syscall_handler(registers_t *regs)
{

    uint32_t syscall_no = regs->eax;

    switch (syscall_no)
    {
    case 0:
        kprintf("[Syscall] Test called! Arg1: %d, Arg2: %d\n", regs->ebx, regs->ecx);
        break;

    case 1:
        kprintf("[Syscall] Write called: %s\n", (char *)regs->ebx);
        break;

    default:
        kprintf("[Syscall] Unknown syscall #%d\n", syscall_no);
        break;
    }
}

void syscall_init(void)
{
    kprintf("Syscalls initialized.\n");
}