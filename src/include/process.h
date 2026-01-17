#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include <stdint.h>

#define PROCESS_STATE_READY 0
#define PROCESS_STATE_RUNNING 1
#define PROCESS_STATE_ZOMBIE 2

typedef struct process
{
    int pid;
    char name[32];
    uintptr_t stack_ptr;
    uintptr_t stack_base;

    uintptr_t kstack_base;
    uintptr_t kstack_top;

    int state;
    struct process *next;
} process_t;

void scheduler_init(void);
void scheduler_add_task(void (*entry_point)(), const char *name);
uintptr_t scheduler_schedule(uintptr_t current_stack_ptr);
void scheduler_exit_current(int code);

#endif