#include "process.h"
#include "heap.h"
#include "string.h"
#include "stdio.h"
#include "hal.h"

process_t *current_process = 0;
process_t *process_list = 0;

static int next_pid = 1;

static void str_copy(char *dest, const char *src) {
    int i = 0;
    while (src[i]) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = 0;
}

void scheduler_init(void) {
    process_t *kernel_proc = (process_t*)kmalloc(sizeof(process_t));
    memset(kernel_proc, 0, sizeof(process_t));
    
    kernel_proc->pid = 0;
    str_copy(kernel_proc->name, "Kernel");
    kernel_proc->state = PROCESS_RUNNING;

    kernel_proc->next = kernel_proc;

    process_list = kernel_proc;
    current_process = kernel_proc;

    kprintf("[Scheduler] Initialized. Kernel is PID 0.\n");
}

void scheduler_add_task(void (*entry_point)(), const char* name) {
    hal_cpu_disable_interrupts();

    process_t *new_proc = (process_t*)kmalloc(sizeof(process_t));
    memset(new_proc, 0, sizeof(process_t));

    uint32_t stack_size = 4096;
    void *stack_addr = kmalloc(stack_size);
    memset(stack_addr, 0, stack_size);

    uint32_t *esp = (uint32_t*)((uintptr_t)stack_addr + stack_size);

    // [EFLAGS] [CS] [EIP]
    *--esp = 0x202;   // EFLAGS
    *--esp = 0x08;    // CS
    *--esp = (uint32_t)entry_point; // EIP


    *--esp = 0; // Error Code
    *--esp = 0; // Int No

    *--esp = 0; // EAX
    *--esp = 0; // ECX
    *--esp = 0; // EDX
    *--esp = 0; // EBX
    *--esp = 0; // ESP
    *--esp = 0; // EBP
    *--esp = 0; // ESI
    *--esp = 0; // EDI

    *--esp = 0x10; // DS/ES/FS/GS

    new_proc->pid = next_pid++;
    str_copy(new_proc->name, name);
    new_proc->stack_ptr = (uintptr_t)esp;
    new_proc->stack_base = (uintptr_t)stack_addr;
    new_proc->state = PROCESS_READY;

    process_t *last = process_list;
    while(last->next != process_list) {
        last = last->next;
    }
    last->next = new_proc;
    new_proc->next = process_list;

    kprintf("[Scheduler] Task Created: PID %d (%s)\n", new_proc->pid, name);

    hal_cpu_enable_interrupts();
}

uintptr_t scheduler_schedule(uintptr_t current_stack_ptr) {
    if (!current_process) return current_stack_ptr;

    current_process->stack_ptr = current_stack_ptr;

    current_process = current_process->next;

    return current_process->stack_ptr;
}

void scheduler_switch() {
}