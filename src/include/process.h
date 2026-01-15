#ifndef KERNEL_PROCESS_H
#define KERNEL_PROCESS_H

#include <stdint.h>

typedef enum
{
    PROCESS_STATE_READY,
    PROCESS_STATE_RUNNING,
    PROCESS_STATE_ZOMBIE,
    PROCESS_STATE_KILLED
} process_state_t;

typedef struct process
{
    int pid;
    char name[32];
    uintptr_t stack_ptr;
    uintptr_t stack_base;

    process_state_t state;
    struct process *next;
} process_t;

void scheduler_init(void);
void scheduler_add_task(void (*entry_point)(), const char *name);
uintptr_t scheduler_schedule(uintptr_t current_stack_ptr);

void scheduler_exit_current(int code);

#endif