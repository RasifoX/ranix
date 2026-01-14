#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

typedef enum
{
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_DEAD
} process_state_t;

typedef struct
{
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags, useresp, ss;
} context_t;

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

#endif