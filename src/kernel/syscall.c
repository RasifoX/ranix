#include "syscall.h"
#include "syscall_nums.h"
#include "stdio.h"
#include "hal.h"
#include "process.h"
#include "keyboard.h"

void syscall_handler(registers_t *regs)
{
    uint32_t syscall_no = regs->eax;

    switch (syscall_no)
    {
    case 0:
        kprintf("[Syscall] Test called! Args: %d, %d, %d\n", regs->ebx, regs->ecx, regs->edx);
        regs->eax = 0;
        break;

    case SYS_EXIT:
        scheduler_exit_current(regs->ebx);
        break;

    case SYS_READ:
        if (regs->ebx == 0)
        {
            char *buf = (char *)regs->ecx;
            uint32_t count = regs->edx;
            uint32_t i = 0;

            while (i < count && keyboard_has_chars())
            {
                buf[i] = keyboard_get_char();
                i++;
            }

            regs->eax = i;
        }
        else
        {
            regs->eax = -1;
        }
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
            regs->eax = len;
        }
        break;

    default:
        kprintf("[Syscall] Unknown: %d\n", syscall_no);
        regs->eax = -1;
        break;
    }
}
void syscall_init(void)
{
    kprintf("Syscalls initialized.\n");
}