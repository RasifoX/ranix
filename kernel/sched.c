#include <kernel/proc.h>
#include <kernel/hal.h>
#include <kernel/tty/fb_tty.h>
#include <kernel/elf.h>
#include <kernel/kprintf.h>
#include <kernel/irq.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>
#include <kernel/vfs.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>
#include <asm/mmu.h>

static process_t *proctab[PROC_MAX];
static process_t  idle_proc;
static uint8_t    idle_kstack[KSTACK_SIZE] __attribute__((aligned(16)));
static uint8_t    irq_stack[KSTACK_SIZE] __attribute__((aligned(16)));
static int        proc_cnt  = 0;

static spinlock_t sched_lock = SPINLOCK_INIT;

/* O(1) Runqueue: simple head/tail linked list */
static process_t *rq_head = NULL;
static process_t *rq_tail = NULL;

volatile int      need_resched  = 0;
volatile uint64_t kernel_ticks  = 0;

/* Internal: must hold sched_lock */
static void runqueue_add(process_t *p)
{
	if (p->pid == 0) return;
	p->sched_next = NULL;
	if (!rq_tail) {
		rq_head = rq_tail = p;
	} else {
		rq_tail->sched_next = p;
		rq_tail = p;
	}
}

/* Internal: must hold sched_lock */
static process_t *runqueue_pop(void)
{
	if (!rq_head) return NULL;
	process_t *p = rq_head;
	rq_head = p->sched_next;
	if (!rq_head) rq_tail = NULL;
	p->sched_next = NULL;
	return p;
}

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
	p->entry = idle_loop;
	p->next  = p;
	p->kstack = idle_kstack;
	p->ticks_remaining = SCHED_QUANTUM;
	memcpy(p->name, "idle", 5);

	uint8_t *sp_top = idle_kstack + KSTACK_SIZE;
	arch_context_t *ctx = (arch_context_t *)(sp_top - sizeof(arch_context_t));
	arch_context_init(ctx, idle_loop, (uint64_t)sp_top);
	p->ctx = ctx;

	cpu_context_t *cpu = arch_get_cpu_ctx();
	cpu->self       = cpu;
	cpu->current    = p;
	cpu->kstack_top = sp_top;
	cpu->irq_stack  = irq_stack + KSTACK_SIZE;
	cpu->cpu_id     = 0;
	cpu->magic      = 0xCAFEBABE;

	proctab[0] = p;
	proc_cnt   = 1;
}

void sched_tick(void)
{
	kernel_ticks++;
	process_t *current = task_current();
	if (!current) return;

	current->ticks++;

	uint64_t f = spin_lock_irq(&sched_lock);
	if (current->pid != 0) {
		if (--current->ticks_remaining <= 0) {
			need_resched = 1;
		}
	} else if (rq_head) {
		need_resched = 1;
	}
	spin_unlock_irq(&sched_lock, f);
	fb_tty_tick();
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
	void *kstack_phys = buddy_alloc(1);
	if (!kstack_phys) { 
		spin_unlock_irq(&sched_lock, f);
		buddy_free((uint64_t)p, 0); 
		return -1; 
	}
	p->kstack = (uint8_t *)PHYS_TO_VIRT(kstack_phys);

	p->fds = fd_table_create();
	p->cwd[0] = '/'; p->cwd[1] = '\0';
	p->umask = 022;

	uint8_t *sp_top = p->kstack + KSTACK_SIZE;
	arch_context_t *ctx = (arch_context_t *)(sp_top - sizeof(arch_context_t));
	arch_context_init(ctx, fn, (uint64_t)sp_top);
	p->ctx = ctx;

	p->next = proctab[0]->next;
	proctab[0]->next = p;
	runqueue_add(p);

	proctab[proc_cnt++] = p;
	spin_unlock_irq(&sched_lock, f);
	return p->pid;
}

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

	p->vm = vm_create();
	if (!p->vm) goto fail;

	elf_info_t ei = {0};
	if (elf_load(p->vm, elf_data, elf_len, &ei) != 0) {
		kprintf("[sched] elf_load failed\n");
		goto fail;
	}

	p->brk          = ei.load_end;
	p->mmap_next    = ARCH_USER_MMAP_BASE;
	p->vfork_parent = -1;
	p->reaped       = 0;

	for (int i = 0; i < ARCH_USER_STACK_PAGES; i++) {
		void *spage = buddy_alloc(0);
		if (!spage) goto fail;
		memset(spage, 0, PAGE_SIZE);
		uint64_t sva = ARCH_USER_STACK_TOP - (uint64_t)(ARCH_USER_STACK_PAGES - i) * PAGE_SIZE;
		vm_map(p->vm, sva, (uint64_t)spage, VM_USER | VM_READ | VM_WRITE);
	}

	void *kstack_phys = buddy_alloc(1);
	if (!kstack_phys) goto fail;
	p->kstack = (uint8_t *)PHYS_TO_VIRT(kstack_phys);

	uint8_t *ksp = p->kstack + KSTACK_SIZE;
	arch_context_t *ctx = (arch_context_t *)(ksp - sizeof(arch_context_t));
	arch_context_init_user(ctx, ei.entry, ARCH_USER_STACK_TOP - 64);
	p->ctx = ctx;
	p->fds = fd_table_create();
	memcpy(p->cwd, current->cwd, 256);
	p->umask = current->umask;

	p->next = proctab[0]->next;
	proctab[0]->next = p;
	runqueue_add(p);

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
	memcpy(child->cwd, parent->cwd, 256);
	child->umask = parent->umask;

	void *child_kstack_phys = buddy_alloc(1);
	if (!child_kstack_phys) goto fail;
	child->kstack = (uint8_t *)PHYS_TO_VIRT(child_kstack_phys);

	uint8_t *ksp = child->kstack + KSTACK_SIZE;
	arch_context_t *cctx = (arch_context_t *)(ksp - sizeof(arch_context_t));
	memcpy(cctx, parent->ctx, sizeof(arch_context_t));
	
	CTX_SYSCALL_RET(cctx, 0);
	child->ctx = cctx;
	child->ticks_remaining = SCHED_QUANTUM;

	child->next  = proctab[0]->next;
	proctab[0]->next = child;
	runqueue_add(child);

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
	memcpy(child->cwd, parent->cwd, 256);
	child->umask        = parent->umask;

	void *child_kstack_phys = buddy_alloc(1);
	if (!child_kstack_phys) { 
		spin_unlock_irq(&sched_lock, f);
		buddy_free((uint64_t)child, 0); 
		return -1; 
	}
	child->kstack = (uint8_t *)PHYS_TO_VIRT(child_kstack_phys);

	uint8_t *ksp = child->kstack + KSTACK_SIZE;
	arch_context_t *cctx = (arch_context_t *)(ksp - sizeof(arch_context_t));
	memcpy(cctx, parent->ctx, sizeof(arch_context_t));
	CTX_SYSCALL_RET(cctx, 0);
	child->ctx = cctx;

	child->next    = proctab[0]->next;
	proctab[0]->next = child;
	runqueue_add(child);

	if (parent->ctx) {
		CTX_SYSCALL_RET(parent->ctx, child->pid);
	}

	parent->state = PROC_SLEEPING;
	need_resched  = 1;
	
	spin_unlock_irq(&sched_lock, f);
	return child->pid;
}

pid_t proc_clone_thread(uint64_t newsp, uint64_t tls, uint32_t *ctid)
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

	child->pid          = proc_cnt;
	child->ppid         = parent->pid;
	child->state        = PROC_READY;
	child->uid          = parent->uid;
	child->gid          = parent->gid;
	child->brk          = parent->brk;
	child->mmap_next    = parent->mmap_next;
	child->vfork_parent = -1;

	int nlen = 0;
	while (parent->name[nlen] && nlen < PROC_NAME_MAX - 3) nlen++;
	memcpy(child->name, parent->name, nlen);
	child->name[nlen] = '\0';

	/* Threads share the VM space */
	child->vm = vm_get(parent->vm);
	if (!child->vm) goto fail;

	/* Threads share the fd table */
	child->fds = parent->fds;
	/* bump refcounts for each open fd so close() works correctly */
	if (child->fds) {
		for (int i = 0; i < FD_MAX; i++) {
			if (child->fds->files[i])
				child->fds->files[i]->refcount++;
		}
	}

	memcpy(child->cwd, parent->cwd, sizeof(child->cwd));
	child->umask = parent->umask;

	void *ks = buddy_alloc(1);
	if (!ks) goto fail;
	child->kstack = (uint8_t *)PHYS_TO_VIRT(ks);

	uint8_t *ksp = child->kstack + KSTACK_SIZE;
	arch_context_t *cctx = (arch_context_t *)(ksp - sizeof(arch_context_t));
	memcpy(cctx, parent->ctx, sizeof(arch_context_t));

	/* Thread starts at clone() return point with return value 0 */
	CTX_SYSCALL_RET(cctx, 0);

	/* Set new stack pointer */
	if (newsp) {
#if defined(__aarch64__)
		cctx->sp = newsp;
#elif defined(__x86_64__)
		cctx->rsp = newsp;
#endif
	}

	/* Set TLS */
	if (tls) {
		child->tls_base = tls;
#if defined(__aarch64__)
		/* will be written to tpidr_el0 on context switch */
#endif
	}

	/* Write child TID to ctid if requested */
	if (ctid) {
		uint32_t tid = (uint32_t)child->pid;
		/* we can't use copy_to_user here (no current context switch yet),
		 * but ctid is in the shared VM so direct write works */
		*ctid = tid;
	}

	child->ctx             = cctx;
	child->ticks_remaining = SCHED_QUANTUM;

	child->next      = proctab[0]->next;
	proctab[0]->next = child;
	runqueue_add(child);
	proctab[proc_cnt++] = child;

	spin_unlock_irq(&sched_lock, f);
	return child->pid;

fail:
	spin_unlock_irq(&sched_lock, f);
	if (child->vm) vm_destroy(child->vm);
	buddy_free((uint64_t)child, 0);
	return -1;
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

	if (prev->state == PROC_RUNNING) {
		prev->state = PROC_READY;
		runqueue_add(prev);
	}

	process_t *next = runqueue_pop();
	if (!next) {
		next = &idle_proc;
	}

	next->state = PROC_RUNNING;
	next->ticks_remaining = SCHED_QUANTUM;

	arch_save_tls(prev);

	cpu_context_t *cpu = arch_get_cpu_ctx();
	cpu->current = next;
	/* IMPORTANT: update the syscall kstack pointer */
	cpu->kstack_top = next->kstack + KSTACK_SIZE;

	arch_restore_tls(next);

	spin_unlock_irq(&sched_lock, f);
}

void yield(void)      { arch_yield(); }
void task_sleep(void) { 
	uint64_t f = spin_lock_irq(&sched_lock);
	task_current()->state = PROC_SLEEPING; 
	spin_unlock_irq(&sched_lock, f);
	arch_yield(); 
}

void task_exit(void)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *p = task_current();
	p->state = PROC_DEAD;

	if (p->vfork_parent >= 0) {
		process_t *pp = (p->vfork_parent < proc_cnt) ? proctab[p->vfork_parent] : NULL;
		if (pp && pp->state == PROC_SLEEPING) {
			pp->state = PROC_READY;
			runqueue_add(pp);
			need_resched = 1;
		}
		p->vfork_parent = -1;
	}

	process_t *parent = (p->ppid < proc_cnt) ? proctab[p->ppid] : NULL;
	if (parent) {
		if (parent->state == PROC_SLEEPING) {
			parent->state = PROC_READY;
			runqueue_add(parent);
			need_resched = 1;
		}
		parent->sig_pending |= (1UL << (SIGCHLD - 1));
	}

	spin_unlock_irq(&sched_lock, f);
	arch_yield();
}

void task_wake(process_t *p)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	if (p && p->state == PROC_SLEEPING) {
		p->state = PROC_READY;
		runqueue_add(p);
		need_resched = 1;
	}
	spin_unlock_irq(&sched_lock, f);
}

void task_signal(process_t *p, int sig)
{
	if (!p || sig <= 0 || sig > NSIG) return;
	uint64_t f = spin_lock_irq(&sched_lock);
	p->sig_pending |= (1UL << (sig - 1));
	if (p->state == PROC_SLEEPING) {
		p->state = PROC_READY;
		runqueue_add(p);
		need_resched = 1;
	}
	spin_unlock_irq(&sched_lock, f);
}

void signal_dispatch(arch_context_t *tf)
{
	(void)tf;
	process_t *p = task_current();
	if (!p) return;

	uint64_t mask = p->sig_pending & ~p->sig_mask;
	if (!mask) return;

	int sig = 0;
	for (int i = 0; i < NSIG; i++) {
		if (mask & (1UL << i)) {
			sig = i + 1;
			break;
		}
	}

	if (sig == SIGKILL) {
		p->sig_pending &= ~(1UL << (sig - 1));
		task_exit();
	}
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
