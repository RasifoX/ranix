#ifndef SCHED_H
#define SCHED_H

#include <uapi/types.h>
#include <asm/context.h>
#include <kernel/mm/vm.h>

#define TASK_STACK_SIZE  8192
#define TASK_MAX         16
#define TASK_NAME_MAX    16

typedef enum {
	TASK_RUNNING  = 0,
	TASK_READY    = 1,
	TASK_DEAD     = 2,
	TASK_SLEEPING = 3,
} task_state_t;

typedef struct task {
	uint64_t        id;
	uint64_t        state;
	arch_context_t *ctx;        /* offset 16 — hardcoded in exceptions_asm.S */
	void          (*entry)(void);
	uint8_t        *stack_base;
	char            name[TASK_NAME_MAX];
	struct task    *next;
	vm_space_t     *vm;         /* NULL = kernel task, uses kernel page tables */
} task_t;

void    sched_init(void);
int     task_create(const char *name, void (*fn)(void));
void    schedule(void);
void    yield(void);
void    task_sleep(void);
void    task_wake(task_t *t);
void    task_exit(void);
task_t *task_current(void);
int     task_count(void);
task_t *task_get(int index);

arch_context_t *sched_get_current_ctx(void);

#endif
