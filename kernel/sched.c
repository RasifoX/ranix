#include <kernel/proc.h>
#include <kernel/hal.h>
#include <kernel/elf.h>
#include <kernel/kprintf.h>
#include <kernel/irq.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/mmu.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>

static process_t *proctab[PROC_MAX];
static process_t  idle_proc;
static uint8_t    idle_kstack[KSTACK_SIZE] __attribute__((aligned(16)));
static int        proc_cnt  = 0;

static spinlock_t sched_lock = SPINLOCK_INIT;

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

	/* Initialize per-CPU structure */
	cpu_context_t *cpu = arch_get_cpu_ctx();
	cpu->current = p;
	cpu->cpu_id  = 0;
	cpu->magic   = 0xCAFEBABE;

	proctab[0] = p;
	proc_cnt   = 1;
}

void sched_tick(void)
{
	process_t *current = task_current();
	if (!current) return;

	uint64_t f = spin_lock_irq(&sched_lock);
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
	spin_unlock_irq(&sched_lock, f);
}

pid_t proc_create(const char *name, void (*fn)(void))
{
	uint64_t f = spin_lock_irq(&sched_lock);
	if (proc_cnt >= PROC_MAX) {
		spin_unlock_irq(&sched_lock, f);
		return -1;
	}

	process_t *p = buddy_alloc(0);
	if (!p) {
		spin_unlock_irq(&sched_lock, f);
		return -1;
	}
	memset(p, 0, sizeof(*p));

	process_t *current = task_current();
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
	p->kstack = buddy_alloc(1);
	if (!p->kstack) { 
		spin_unlock_irq(&sched_lock, f);
		buddy_free((uint64_t)p, 0); 
		return -1; 
	}

	p->fds = fd_table_create();

	uint8_t *sp_top = p->kstack + KSTACK_SIZE;
	arch_context_t *ctx = (arch_context_t *)(sp_top - sizeof(arch_context_t));
	arch_context_init(ctx, fn, (uint64_t)sp_top);
	p->ctx = ctx;

	/* insert into scheduler ring */
	p->next = current->next;
	current->next = p;

	proctab[proc_cnt++] = p;
	spin_unlock_irq(&sched_lock, f);
	return p->pid;
}

#define USER_CODE_BASE   0x400000UL
#define USER_STACK_TOP   0x800000UL
#define USER_STACK_PAGES 2
#define USER_MMAP_BASE   0x10000000UL

pid_t proc_exec(const char *name, const void *elf_data, size_t elf_len)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	if (proc_cnt >= PROC_MAX) {
		spin_unlock_irq(&sched_lock, f);
		return -1;
	}

	process_t *p = buddy_alloc(0);
	if (!p) {
		spin_unlock_irq(&sched_lock, f);
		return -1;
	}
	memset(p, 0, sizeof(*p));

	process_t *current = task_current();
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

	p->kstack = buddy_alloc(1);
	if (!p->kstack) goto fail;

	uint8_t *ksp = p->kstack + KSTACK_SIZE;
	arch_context_t *ctx = (arch_context_t *)(ksp - sizeof(arch_context_t));
	arch_context_init_user(ctx, entry, USER_STACK_TOP - 64);
	p->ctx = ctx;
	p->fds = fd_table_create();

	p->next = current->next;
	current->next = p;
	proctab[proc_cnt++] = p;
	spin_unlock_irq(&sched_lock, f);
	return p->pid;

fail:
	spin_unlock_irq(&sched_lock, f);
	if (p->vm) vm_destroy(p->vm);
	if (p->fds) fd_table_destroy(p->fds);
	buddy_free((uint64_t)p, 0);
	return -1;
}

pid_t proc_fork(void)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	if (proc_cnt >= PROC_MAX) {
		spin_unlock_irq(&sched_lock, f);
		return -1;
	}

	process_t *parent = task_current();
	process_t *child  = buddy_alloc(0);
	if (!child) {
		spin_unlock_irq(&sched_lock, f);
		return -1;
	}
	memset(child, 0, sizeof(*child));

	child->pid       = proc_cnt;
	child->ppid      = parent->pid;
	child->state     = PROC_READY;
	child->uid       = parent->uid;
	child->gid       = parent->gid;
	child->brk       = parent->brk;
	child->mmap_next = parent->mmap_next;
	child->reaped    = 0;
	child->vfork_parent = -1;

	int nlen = 0;
	while (parent->name[nlen] && nlen < PROC_NAME_MAX - 3) nlen++;
	memcpy(child->name, parent->name, nlen);
	child->name[nlen++] = '_';
	child->name[nlen++] = 'c';
	child->name[nlen] = '\0';

	child->vm = vm_copy(parent->vm);
	if (!child->vm) goto fail;

	child->fds = fd_table_copy(parent->fds);
	if (!child->fds) goto fail;

	child->kstack = buddy_alloc(1);
	if (!child->kstack) goto fail;

	uint8_t *ksp = child->kstack + KSTACK_SIZE;
	arch_context_t *cctx = (arch_context_t *)(ksp - sizeof(arch_context_t));
	memcpy(cctx, parent->ctx, sizeof(arch_context_t));
	
	CTX_SYSCALL_RET(cctx, 0);
	child->ctx = cctx;
	child->ticks_remaining = SCHED_QUANTUM;

	child->next  = parent->next;
	parent->next = child;

	proctab[proc_cnt++] = child;
	spin_unlock_irq(&sched_lock, f);
	return child->pid;

fail:
	spin_unlock_irq(&sched_lock, f);
	if (child->vm) vm_destroy(child->vm);
	if (child->fds) fd_table_destroy(child->fds);
	buddy_free((uint64_t)child, 0);
	return -1;
}

pid_t proc_vfork(void)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	if (proc_cnt >= PROC_MAX) {
		spin_unlock_irq(&sched_lock, f);
		return -1;
	}

	process_t *parent = task_current();
	process_t *child = buddy_alloc(0);
	if (!child) {
		spin_unlock_irq(&sched_lock, f);
		return -1;
	}
	memset(child, 0, sizeof(*child));

	child->pid          = proc_cnt;
	child->ppid         = parent->pid;
	child->state        = PROC_READY;
	child->uid          = parent->uid;
	child->gid          = parent->gid;
	child->vm           = parent->vm;
	child->brk          = parent->brk;
	child->mmap_next    = parent->mmap_next;
	child->vfork_parent = parent->pid;
	child->reaped       = 0;
	child->fds          = fd_table_copy(parent->fds);
	memcpy(child->name, parent->name, PROC_NAME_MAX);

	child->kstack = buddy_alloc(1);
	if (!child->kstack) { 
		spin_unlock_irq(&sched_lock, f);
		buddy_free((uint64_t)child, 0); 
		return -1; 
	}

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

	parent->state = PROC_SLEEPING;
	need_resched  = 1;
	
	spin_unlock_irq(&sched_lock, f);
	return child->pid;
}

process_t *proc_find(pid_t pid)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *p = (pid < 0 || pid >= proc_cnt) ? NULL : proctab[pid];
	spin_unlock_irq(&sched_lock, f);
	return p;
}

void proc_wait(pid_t pid)
{
	while (1) {
		uint64_t f = spin_lock_irq(&sched_lock);
		process_t *p = (pid < 0 || pid >= proc_cnt) ? NULL : proctab[pid];
		if (!p || p->state == PROC_DEAD) {
			spin_unlock_irq(&sched_lock, f);
			break;
		}
		spin_unlock_irq(&sched_lock, f);
		task_sleep();
	}
}

void schedule(void)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *prev = task_current();

	if (prev->state == PROC_RUNNING)
		prev->state = PROC_READY;

	process_t *next = prev->next;
	while (next != prev) {
		if (next->state == PROC_READY)
			break;
		next = next->next;
	}

	if (next == prev || next->state != PROC_READY) {
		if (prev->state == PROC_READY && prev != &idle_proc) {
			next = &idle_proc;
		} else if (prev == &idle_proc) {
			prev->state = PROC_RUNNING;
			spin_unlock_irq(&sched_lock, f);
			return;
		} else {
			next = &idle_proc;
		}
	}

	next->state = PROC_RUNNING;
	next->ticks_remaining = SCHED_QUANTUM;
	arch_get_cpu_ctx()->current = next;
	spin_unlock_irq(&sched_lock, f);
}

void yield(void)      { arch_yield(); }
void task_sleep(void) { task_current()->state = PROC_SLEEPING; arch_yield(); }

void task_exit(void)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *p = task_current();
	p->state = PROC_DEAD;

	if (p->vfork_parent >= 0) {
		process_t *pp = (p->vfork_parent < proc_cnt) ? proctab[p->vfork_parent] : NULL;
		if (pp && pp->state == PROC_SLEEPING) {
			pp->state = PROC_READY;
			need_resched = 1;
		}
		p->vfork_parent = -1;
	}

	process_t *parent = (p->ppid < proc_cnt) ? proctab[p->ppid] : NULL;
	if (parent && parent->state == PROC_SLEEPING) {
		parent->state = PROC_READY;
		need_resched = 1;
	}

	spin_unlock_irq(&sched_lock, f);
	arch_yield();
}

void task_wake(process_t *p)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	if (p && p->state == PROC_SLEEPING) {
		p->state = PROC_READY;
		need_resched = 1;
	}
	spin_unlock_irq(&sched_lock, f);
}

arch_context_t *sched_get_current_ctx(void)
{
	return task_current()->ctx;
}

process_t *task_current(void)
{
	return arch_get_cpu_ctx()->current;
}

int        task_count(void)   { return proc_cnt; }

process_t *task_get(int i)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *p = (i < 0 || i >= proc_cnt) ? NULL : proctab[i];
	spin_unlock_irq(&sched_lock, f);
	return p;
}