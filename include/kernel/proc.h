#ifndef PROC_H
#define PROC_H

#include <uapi/types.h>
#include <asm/context.h>
#include <kernel/mm/vm.h>
#include <kernel/vfs.h>

#define PROC_MAX       256
#define PROC_NAME_MAX  256
#define KSTACK_SIZE    8192

typedef enum {
	PROC_RUNNING  = 0,
	PROC_READY    = 1,
	PROC_DEAD     = 2,
	PROC_SLEEPING = 3,
	PROC_ZOMBIE   = 4,   /* exited, waiting for parent wait() */
} proc_state_t;

/*
 * Process Control Block.
 *
 * Field order is FIXED through `vm` — exceptions_asm.S encodes
 * offset 16 for `ctx` directly. Do not reorder the first 8 fields.
 */
typedef struct process {
	/* ── scheduler / asm-visible ─────────────── */
	uint64_t        pid;          /* offset 0  */
	uint64_t        state;        /* offset 8  */
	arch_context_t *ctx;          /* offset 16 — hardcoded in exceptions_asm.S */
	void          (*entry)(void); /* offset 24 */
	uint8_t        *kstack;       /* offset 32 */
	char            name[PROC_NAME_MAX]; /* offset 40 */
	struct process *next;         /* scheduler list */
	vm_space_t     *vm;

	/* ── process metadata ────────────────────── */
	uint64_t        brk;
	uint64_t        mmap_next;
	pid_t           ppid;
	uid_t           uid;
	gid_t           gid;
	int             exit_code;

	/* ── open files ──────────────────────────── */
	fd_table_t     *fds;

	/* ── virtual memory regions ──────────────── */
	vma_t          *vmas;
} process_t;

/* scheduler */
void       sched_init(void);
void       schedule(void);
void       yield(void);
void       task_sleep(void);
void       task_wake(process_t *p);
void       task_exit(void);
process_t *task_current(void);
int        task_count(void);
process_t *task_get(int index);
arch_context_t *sched_get_current_ctx(void);

/* process lifecycle */
pid_t      proc_create(const char *name, void (*fn)(void));
pid_t      proc_exec(const char *name, const void *elf_data, size_t elf_len);
process_t *proc_find(pid_t pid);
void       proc_wait(pid_t pid);

/* legacy alias — keeps old callers working */
typedef process_t task_t;
#define task_create(name, fn)  proc_create(name, fn)
#define TASK_MAX    PROC_MAX
#define TASK_RUNNING  PROC_RUNNING
#define TASK_READY    PROC_READY
#define TASK_DEAD     PROC_DEAD
#define TASK_SLEEPING PROC_SLEEPING

#endif
