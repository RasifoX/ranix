#include "process.h"
#include "heap.h"
#include "string.h"
#include "stdio.h"
#include "hal.h"
#include "gdt.h"

process_t *process_list = 0;
process_t *current_process = 0;
static int next_pid = 1;

extern uint32_t stack_bottom;

static process_t *get_next_ready_process()
{
    if (!current_process)
        return 0;

    process_t *p = current_process->next;
    process_t *start = p;
    do
    {
        if (p->state == PROCESS_STATE_READY || p->state == PROCESS_STATE_RUNNING)
        {
            return p;
        }
        p = p->next;
    } while (p != start);

    return 0;
}

void scheduler_init(void)
{
    process_t *kernel_proc = (process_t *)kmalloc(sizeof(process_t));
    memset(kernel_proc, 0, sizeof(process_t));

    kernel_proc->pid = 0;

    char *name = "Kernel";
    int i = 0;
    while (name[i])
    {
        kernel_proc->name[i] = name[i];
        i++;
    }

    kernel_proc->state = PROCESS_STATE_RUNNING;
    kernel_proc->next = kernel_proc;

    uintptr_t boot_stack_top = (uintptr_t)&stack_bottom + 16384;

    kernel_proc->stack_base = (uintptr_t)&stack_bottom;
    kernel_proc->kstack_base = (uintptr_t)&stack_bottom;
    kernel_proc->kstack_top = boot_stack_top;

    process_list = kernel_proc;
    current_process = kernel_proc;

    kprintf("[Scheduler] Initialized.\n");
}

void scheduler_add_task(void (*entry_point)(), const char *name)
{
    hal_cpu_disable_interrupts();

    process_t *new_proc = (process_t *)kmalloc(sizeof(process_t));
    memset(new_proc, 0, sizeof(process_t));

    uint32_t stack_size = 4096;
    void *u_stack = kmalloc(stack_size);
    memset(u_stack, 0, stack_size);

    uint32_t *esp = (uint32_t *)((uintptr_t)u_stack + stack_size);

    void *k_stack = kmalloc(stack_size);
    memset(k_stack, 0, stack_size);

    *--esp = 0x202;                 // EFLAGS
    *--esp = 0x08;                  // CS
    *--esp = (uint32_t)entry_point; // EIP

    *--esp = 0; // Dummy Error Code
    *--esp = 0; // Dummy Interrupt Number

    *--esp = 0; // EAX
    *--esp = 0; // ECX
    *--esp = 0; // EDX
    *--esp = 0; // EBX
    *--esp = 0; // ESP
    *--esp = 0; // EBP
    *--esp = 0; // ESI
    *--esp = 0; // EDI

    *--esp = 0x10; // DS

    new_proc->pid = next_pid++;

    int k = 0;
    while (name[k])
    {
        new_proc->name[k] = name[k];
        k++;
    }

    new_proc->stack_ptr = (uintptr_t)esp;
    new_proc->stack_base = (uintptr_t)u_stack;

    new_proc->kstack_base = (uintptr_t)k_stack;
    new_proc->kstack_top = (uintptr_t)k_stack + stack_size;

    new_proc->state = PROCESS_STATE_READY;

    process_t *last = process_list;
    while (last->next != process_list)
    {
        last = last->next;
    }
    last->next = new_proc;
    new_proc->next = process_list;

    kprintf("[Scheduler] Task Created: PID %d (%s)\n", new_proc->pid, name);
    hal_cpu_enable_interrupts();
}

uintptr_t scheduler_schedule(uintptr_t current_stack_ptr)
{
    if (!current_process)
        return current_stack_ptr;

    current_process->stack_ptr = current_stack_ptr;

    if (current_process->state == PROCESS_STATE_RUNNING)
    {
        current_process->state = PROCESS_STATE_READY;
    }

    process_t *next = get_next_ready_process();
    if (!next)
        next = current_process;

    current_process = next;
    current_process->state = PROCESS_STATE_RUNNING;

    if (current_process->pid != 0)
    {
        tss_set_stack(0x10, current_process->kstack_top);
    }

    return current_process->stack_ptr;
}

void scheduler_exit_current(int code)
{
    kprintf("[Scheduler] Process PID %d (%s) exited with code %d\n",
            current_process->pid, current_process->name, code);

    current_process->state = PROCESS_STATE_ZOMBIE;
    asm volatile("int $0x20");
    while (1)
        ;
}