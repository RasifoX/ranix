#include <kernel/proc.h>
#include <kernel/hal.h>
#include <kernel/elf.h>
#include <kernel/kprintf.h>
#include <kernel/irq.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
static process_t *proctab[PROC_MAX];
static process_t  idle_proc;
static uint8_t    idle_kstack[KSTACK_SIZE] __attribute__((aligned(16)));
static int        proc_cnt  = 0;
static process_t *current   = NULL;

volatile int need_resched = 0;

#define SCHED_QUANTUM  10

/* ── scheduler ───────────────────────────────────────────── */

static void idle_loop(void)
{
	while (1) arch_halt();
}

void sched_init(void)
{
	process_t *p = &idle_proc;
	memset(p, 0, sizeof(*p));
	p->pid   = 0;
	p->state = PROC_RUNNING;
	p->ppid  = 0;
	p->uid   = 0;
	p->gid   = 0;
	p->entry = idle_loop;
	p->next  = p;
	p->kstack = idle_kstack;
	p->ticks_remaining = SCHED_QUANTUM;
	memcpy(p->name, "idle", 5);

	uint8_t *sp_top = idle_kstack + KSTACK_SIZE;
	arch_context_t *ctx = (arch_context_t *)(sp_top - sizeof(arch_context_t));
	arch_context_init(ctx, idle_loop, (uint64_t)sp_top);
	p->ctx = ctx;

	proctab[0] = p;
	current    = p;
	proc_cnt   = 1;
}

void sched_tick(void)
{
	if (!current) return;

	if (current->pid != 0) {
		if (--current->ticks_remaining <= 0) {
			need_resched = 1;
		}
	} else {
		/* If we are in idle, check if anyone else is ready */
		process_t *p = current->next;
		while (p != current) {
			if (p->state == PROC_READY) {
				need_resched = 1;
				break;
			}
			p = p->next;
		}
	}
}

pid_t proc_create(const char *name, void (*fn)(void))
{
	if (proc_cnt >= PROC_MAX)
		return -1;

	process_t *p = buddy_alloc(0);
	if (!p) return -1;
	memset(p, 0, sizeof(*p));

	p->pid   = proc_cnt;
	p->ppid  = current->pid;
	p->state = PROC_READY;
	p->uid   = current->uid;
	p->gid   = current->gid;
	p->entry = fn;
	p->ticks_remaining = SCHED_QUANTUM;

	int nlen = 0;
	while (name[nlen] && nlen < PROC_NAME_MAX - 1) nlen++;
	memcpy(p->name, name, nlen);
	p->name[nlen] = '\0';

	/* kernel stack */
	p->kstack = buddy_alloc(1);   /* 2 pages = 8 KB */
	if (!p->kstack) { buddy_free((uint64_t)p, 0); return -1; }

	p->fds = fd_table_create();

	uint8_t *sp_top = p->kstack + KSTACK_SIZE;
	arch_context_t *ctx = (arch_context_t *)(sp_top - sizeof(arch_context_t));
	arch_context_init(ctx, fn, (uint64_t)sp_top);
	p->ctx = ctx;

	/* insert into scheduler ring */
	p->next = current->next;
	current->next = p;

	proctab[proc_cnt++] = p;
	return p->pid;
}

/* user address space layout */
/* User space lives above the kernel's 1GB identity map (0-0x40000000).
 * On x86_64: above 1GB boot page tables. On aarch64: above L1[3] device. */
#define USER_CODE_BASE   0x400000UL
#define USER_STACK_TOP   0x800000UL
#define USER_STACK_PAGES 2
#define USER_MMAP_BASE   0x10000000UL

pid_t proc_exec(const char *name, const void *elf_data, size_t elf_len)
{
	if (proc_cnt >= PROC_MAX) return -1;

	process_t *p = buddy_alloc(0);
	if (!p) return -1;
	memset(p, 0, sizeof(*p));

	p->pid   = proc_cnt;
	p->ppid  = current->pid;
	p->state = PROC_READY;
	p->uid   = current->uid;
	p->gid   = current->gid;

	int nlen = 0;
	while (name[nlen] && nlen < PROC_NAME_MAX - 1) nlen++;
	memcpy(p->name, name, nlen);
	p->name[nlen] = '\0';

	/* create address space */
	p->vm = vm_create();
	if (!p->vm) goto fail;

	/* load ELF into address space */
	uint64_t entry, load_end = 0;
	if (elf_load(p->vm, elf_data, elf_len, &entry, &load_end) != 0) {
		kprintf("[sched] elf_load failed\n");
		goto fail;
	}

	p->brk          = load_end;
	p->mmap_next    = USER_MMAP_BASE;
	p->vfork_parent = -1;
	p->reaped       = 0;

	/* allocate and map user stack */
	for (int i = 0; i < USER_STACK_PAGES; i++) {
		void *spage = buddy_alloc(0);
		if (!spage) goto fail;
		memset(spage, 0, PAGE_SIZE);
		uint64_t sva = USER_STACK_TOP - (uint64_t)(USER_STACK_PAGES - i) * PAGE_SIZE;
		vm_map(p->vm, sva, (uint64_t)spage, VM_USER | VM_READ | VM_WRITE);
	}

	/* kernel stack for exception handling */
	p->kstack = buddy_alloc(1);
	if (!p->kstack) goto fail;

	uint8_t *ksp = p->kstack + KSTACK_SIZE;
	arch_context_t *ctx = (arch_context_t *)(ksp - sizeof(arch_context_t));
	/* put SP below the stack top so _start's read of argc at [SP] is mapped */
	arch_context_init_user(ctx, entry, USER_STACK_TOP - 64);
	p->ctx = ctx;
	/* fd table — stdin/stdout/stderr as NULL for now */
	p->fds = fd_table_create();

	p->next = current->next;
	current->next = p;
	proctab[proc_cnt++] = p;
	return p->pid;

fail:
	buddy_free((uint64_t)p, 0);
	return -1;
}

pid_t proc_vfork(void)
{
	if (proc_cnt >= PROC_MAX) return -1;

	process_t *parent = current;

	process_t *child = buddy_alloc(0);
	if (!child) return -1;
	memset(child, 0, sizeof(*child));

	child->pid          = proc_cnt;
	child->ppid         = parent->pid;
	child->state        = PROC_READY;
	child->uid          = parent->uid;
	child->gid          = parent->gid;
	child->vm           = parent->vm;   /* shared — child will replace on execve */
	child->brk          = parent->brk;
	child->mmap_next    = parent->mmap_next;
	child->vfork_parent = parent->pid;
	child->reaped       = 0;
	child->fds          = fd_table_create();
	memcpy(child->name, parent->name, PROC_NAME_MAX);

	child->kstack = buddy_alloc(1);
	if (!child->kstack) { buddy_free((uint64_t)child, 0); return -1; }

	/* copy parent's saved context; child returns 0 from vfork */
	uint8_t *ksp = child->kstack + KSTACK_SIZE;
	arch_context_t *cctx = (arch_context_t *)(ksp - sizeof(arch_context_t));
	memcpy(cctx, parent->ctx, sizeof(arch_context_t));
	CTX_SYSCALL_RET(cctx, 0);
	child->ctx = cctx;

	child->next    = parent->next;
	parent->next   = child;
	proctab[proc_cnt++] = child;

	if (parent->ctx) {
		CTX_SYSCALL_RET(parent->ctx, child->pid);
	}

	/* suspend parent until child calls execve or exits */
	parent->state = PROC_SLEEPING;
	need_resched  = 1;
	
	return child->pid;
}

process_t *proc_find(pid_t pid)
{
	if (pid < 0 || pid >= proc_cnt) return NULL;
	return proctab[pid];
}

void proc_wait(pid_t pid)
{
	process_t *p = proc_find(pid);
	while (p && p->state != PROC_DEAD)
		task_sleep();
}

void schedule(void)
{
	process_t *prev = current;

	if (prev->state == PROC_RUNNING)
		prev->state = PROC_READY;

	process_t *next = prev->next;
	while (next != prev) {
		if (next->state == PROC_READY)
			break;
		next = next->next;
	}

	if (next == prev || next->state != PROC_READY) {
		/* nothing ready — fall back to idle */
		if (prev->state == PROC_READY && prev != &idle_proc) {
			next = &idle_proc;
		} else if (prev == &idle_proc) {
			prev->state = PROC_RUNNING;
			return;
		} else {
			/* prev dead/sleeping and no ready task — go idle */
			next = &idle_proc;
		}
	}

	next->state = PROC_RUNNING;
	next->ticks_remaining = SCHED_QUANTUM;
	current     = next;
}

void yield(void)      { arch_yield(); }
void task_sleep(void) { current->state = PROC_SLEEPING; arch_yield(); }
void task_exit(void)
{
	process_t *p = current;
	p->state = PROC_DEAD;

	if (p->vfork_parent >= 0) {
		process_t *pp = proc_find((pid_t)p->vfork_parent);
		if (pp) task_wake(pp);
		p->vfork_parent = -1;
	}

	process_t *parent = proc_find((pid_t)p->ppid);
	if (parent) task_wake(parent);

	arch_yield();
}

void task_wake(process_t *p)
{
	if (p && p->state == PROC_SLEEPING) {
		p->state = PROC_READY;
		need_resched = 1;
	}
}

arch_context_t *sched_get_current_ctx(void)
{
	return current->ctx;
}

process_t *task_current(void) { return current; }
int        task_count(void)   { return proc_cnt; }

process_t *task_get(int i)
{
	if (i < 0 || i >= proc_cnt) return NULL;
	return proctab[i];
}
