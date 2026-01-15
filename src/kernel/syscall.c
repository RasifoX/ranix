#include <stdint.h>
#include "idt.h"
#include "syscall_nums.h"
#include "syscall.h"
#include "stdio.h"
#include "process.h"

void syscall_handler(registers_t *regs)
{
    uint32_t syscall_no = regs->eax;

    switch (syscall_no)
    {
    case SYS_EXIT:
        scheduler_exit_current(regs->ebx); // EBX = Exit Code
        break;

    case SYS_WRITE:
        if (regs->ebx == 1 || regs->ebx == 2)
        {
            char *buf = (char *)regs->ecx;
            uint32_t len = regs->edx;
            for (uint32_t i = 0; i < len; i++)
            {
                hal_kputc(buf[i]);
            }
        }
        break;

    default:
        kprintf("[Syscall] Unknown: %d\n", syscall_no);
        break;
    }
}

void syscall_init(void)
{
    kprintf("Syscalls initialized.\n");
}