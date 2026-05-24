#include <kernel/proc.h>
#include <kernel/config.h>
#include <kernel/hal.h>
#include <kernel/cmdline.h>
#include <kernel/tick.h>
#include <kernel/elf.h>
#include <kernel/kprintf.h>
#include <kernel/mm/heap.h>
#include <kernel/irq.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>
#include <kernel/vfs.h>
#include <kernel/sync.h>
#include <kernel/lib/string.h>
#include <asm/mmu.h>

static process_t **proctab;
static int         proc_cap = 0;
static process_t   idle_proc;
static uint8_t     idle_kstack[KSTACK_SIZE] __attribute__((aligned(16)));
static uint8_t     irq_stack[KSTACK_SIZE] __attribute__((aligned(16)));
static int         proc_cnt  = 0;

#define PROCTAB_INIT_CAP 64

static int proctab_grow(void)
{
	int new_cap = proc_cap ? proc_cap * 2 : PROCTAB_INIT_CAP;
	process_t **nb = kmalloc((size_t)new_cap * sizeof(process_t *));
	if (!nb) return -1;
	if (proctab) {
		memcpy(nb, proctab, (size_t)proc_cap * sizeof(process_t *));
		kfree(proctab);
	}
	memset(nb + proc_cap, 0, (size_t)(new_cap - proc_cap) * sizeof(process_t *));
	proctab = nb;
	proc_cap = new_cap;
	return 0;
}

static int proctab_ensure(void)
{
	if (proc_cnt < proc_cap) return 0;
	return proctab_grow();
}

/* Scan for recycled NULL slot; extend proc_cnt only when table is full */
static int alloc_pid(void)
{
	for (int i = 1; i < proc_cnt; i++) {
		if (!proctab[i]) return i;
	}
	if (proctab_ensure() != 0) return -1;
	return proc_cnt++;
}

#define KSTACK_CANARY 0xDEADBEEFCAFEBABEULL

static int kstack_size_val  = KSTACK_SIZE;
static int kstack_order_val = 1; /* buddy order; 1 = 2 pages = 8192 bytes */

static uint8_t *kstack_alloc(void)
{
	void *p = buddy_alloc(kstack_order_val);
	if (!p) return NULL;
	uint8_t *ks = (uint8_t *)PHYS_TO_VIRT(p);
	*(uint64_t *)ks = KSTACK_CANARY;
	return ks;
}

static spinlock_t sched_lock = SPINLOCK_INIT;
static int sched_quantum_val = SCHED_QUANTUM;

static tick_fn_t *tick_cbs;
static int        tick_cb_cnt = 0;
static int        tick_cb_cap = 0;

void tick_register(tick_fn_t fn)
{
	if (tick_cb_cnt >= tick_cb_cap) {
		int nc = tick_cb_cap ? tick_cb_cap * 2 : 8;
		tick_fn_t *nb = kmalloc((size_t)nc * sizeof(tick_fn_t));
		if (!nb) return;
		if (tick_cbs) { memcpy(nb, tick_cbs, (size_t)tick_cb_cnt * sizeof(tick_fn_t)); kfree(tick_cbs); }
		tick_cbs = nb; tick_cb_cap = nc;
	}
	tick_cbs[tick_cb_cnt++] = fn;
}

void tick_notify(void)
{
	for (int i = 0; i < tick_cb_cnt; i++)
		tick_cbs[i]();
}

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
	p->ticks_remaining = sched_quantum_val;
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

	proctab_grow();
	proctab[0] = p;
	proc_cnt   = 1;

	{
		char buf[16];
		if (cmdline_get("sched_quantum", buf, sizeof(buf))) {
			int v = 0;
			for (const char *s = buf; *s >= '0' && *s <= '9'; s++)
				v = v * 10 + (*s - '0');
			if (v > 0) sched_quantum_val = v;
		}
		if (cmdline_get("kstack_size", buf, sizeof(buf))) {
			int v = 0;
			for (const char *s = buf; *s >= '0' && *s <= '9'; s++)
				v = v * 10 + (*s - '0');
			/* must be a multiple of PAGE_SIZE and at least 4096 */
			if (v >= 4096 && (v & (v - 1)) == 0) {
				int pages = v / 4096;
				int order = 0;
				while ((1 << order) < pages) order++;
				kstack_size_val  = (1 << order) * 4096;
				kstack_order_val = order;
			}
		}
	}

	tick_register(sched_tick);

	extern void tcp_tick(void);
	tick_register(tcp_tick);

	extern void timerfd_tick(void);
	tick_register(timerfd_tick);
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

	/* Walk circular linked list (sentinel = proctab[0]) under one lock */
	{
		uint64_t lf = spin_lock_irq(&sched_lock);
		process_t *sentinel = proctab[0];
		for (process_t *t = sentinel->next; t && t != sentinel; t = t->next) {
			if (t->itimer_value > 0) {
				t->itimer_value--;
				if (t->itimer_value == 0) {
					t->itimer_value = t->itimer_interval;
					t->sig_pending |= (1ULL << (14 - 1));
					if (t->state == PROC_SLEEPING) { t->state = PROC_READY; runqueue_add(t); need_resched = 1; }
				}
			}
			if (t->state == PROC_SLEEPING && !t->sleeping_on_queue) {
				t->state = PROC_READY;
				runqueue_add(t);
				need_resched = 1;
			}
		}
		spin_unlock_irq(&sched_lock, lf);
	}

}

pid_t proc_create(const char *name, void (*fn)(void))
{
	process_t *p = (process_t *)PHYS_TO_VIRT(buddy_alloc(0));
	if (!p) return -1;
	memset(p, 0, sizeof(*p));

	process_t *current = task_current();
	p->ppid  = current ? current->pid : 0;
	p->state = PROC_READY;
	p->uid   = current ? current->uid : 0;
	p->gid   = current ? current->gid : 0;
	p->entry = fn;
	p->ticks_remaining = sched_quantum_val;

	int nlen = 0;
	while (name[nlen] && nlen < PROC_NAME_MAX - 1) nlen++;
	memcpy(p->name, name, nlen);
	p->name[nlen] = '\0';

	p->kstack = kstack_alloc();
	if (!p->kstack) {
		buddy_free(VIRT_TO_PHYS(p), 0);
		return -1;
	}

	p->fds = fd_table_create();
	p->cwd[0] = '/'; p->cwd[1] = '\0';
	p->umask = 022;

	uint8_t *sp_top = p->kstack + kstack_size_val;
	arch_context_t *ctx = (arch_context_t *)(sp_top - sizeof(arch_context_t));
	arch_context_init(ctx, fn, (uint64_t)sp_top);
	p->ctx = ctx;

	uint64_t f = spin_lock_irq(&sched_lock);
	int pid = alloc_pid();
	if (pid < 0) {
		spin_unlock_irq(&sched_lock, f);
		buddy_free(VIRT_TO_PHYS(p->kstack), kstack_order_val);
		buddy_free(VIRT_TO_PHYS(p), 0);
		return -1;
	}
	p->pid  = pid;
	p->tgid = pid;
	p->pgrp = pid;
	p->next = proctab[0]->next;
	proctab[0]->next = p;
	runqueue_add(p);
	proctab[pid] = p;
	spin_unlock_irq(&sched_lock, f);
	return p->pid;
}

pid_t proc_exec(const char *name, const void *elf_data, size_t elf_len)
{
	process_t *p = (process_t *)PHYS_TO_VIRT(buddy_alloc(0));
	if (!p) return -1;
	memset(p, 0, sizeof(*p));

	process_t *current = task_current();
	p->ppid  = current->pid;
	p->state = PROC_READY;
	/* exec: inherit ruid/rgid; suid/sgid become new euid/egid (no setuid-bit here) */
	p->uid  = current->uid;
	p->euid = current->euid;
	p->suid = current->euid; /* suid = euid after exec */
	p->gid  = current->gid;
	p->egid = current->egid;
	p->sgid = current->egid;
	/* init (PID 1) runs as root with full capabilities */
	if (p->euid == 0) {
		p->cap_permitted   = ~0ULL;
		p->cap_effective   = ~0ULL;
		p->cap_inheritable = ~0ULL;
	}

	int nlen = 0;
	while (name[nlen] && nlen < PROC_NAME_MAX - 1) nlen++;
	memcpy(p->name, name, nlen);
	p->name[nlen] = '\0';

	p->vm = vm_create();
	if (!p->vm) goto fail;

	elf_info_t ei = {0};
	if (elf_load(p->vm, elf_data, elf_len, &ei) != 0) {
		kprintf("exec: elf_load failed\n");
		goto fail;
	}

	p->brk          = ei.load_end;
	p->mmap_next    = ARCH_USER_MMAP_BASE;
	p->vfork_parent = -1;
	p->reaped       = 0;

	for (int i = 0; i < ARCH_USER_STACK_PAGES; i++) {
		void *spage_phys = buddy_alloc(0);
		if (!spage_phys) goto fail;
		memset((void *)PHYS_TO_VIRT(spage_phys), 0, PAGE_SIZE);
		uint64_t sva = ARCH_USER_STACK_TOP - (uint64_t)(ARCH_USER_STACK_PAGES - i) * PAGE_SIZE;
		vm_map(p->vm, sva, (uint64_t)spage_phys, VM_USER | VM_READ | VM_WRITE);
	}
	{
		void *guard_phys = buddy_alloc(0);
		if (guard_phys) {
			memset((void *)PHYS_TO_VIRT(guard_phys), 0, PAGE_SIZE);
			vm_map(p->vm, ARCH_USER_STACK_TOP, (uint64_t)guard_phys, VM_USER | VM_READ | VM_WRITE);
		}
	}

	p->kstack = kstack_alloc();
	if (!p->kstack) goto fail;

	uint8_t *ksp = p->kstack + kstack_size_val;
	arch_context_t *ctx = (arch_context_t *)(ksp - sizeof(arch_context_t));

	/* Switch to new VM to write to user stack */
	extern vm_space_t *kernel_vm_space;
	vm_space_t *old_vm = current->vm;
	arch_vm_switch((uint64_t)p->vm->root);

	uint64_t sp = ARCH_USER_STACK_TOP;
	sp &= ~15ULL;

	sp -= 16;
	uint64_t at_random = sp;
	sp -= 16;
	strcpy((char *)sp, name);
	uint64_t at_execfn = sp;

	sp &= ~15ULL;

	/* auxv */
	sp -= 16; ((uint64_t*)sp)[0] = 0; ((uint64_t*)sp)[1] = 0; /* AT_NULL */
	sp -= 16; ((uint64_t*)sp)[0] = 9; ((uint64_t*)sp)[1] = ei.entry; /* AT_ENTRY */
	sp -= 16; ((uint64_t*)sp)[0] = 6; ((uint64_t*)sp)[1] = PAGE_SIZE; /* AT_PAGESZ */
	sp -= 16; ((uint64_t*)sp)[0] = 5; ((uint64_t*)sp)[1] = ei.phnum; /* AT_PHNUM */
	sp -= 16; ((uint64_t*)sp)[0] = 4; ((uint64_t*)sp)[1] = ei.phentsize; /* AT_PHENT */
	sp -= 16; ((uint64_t*)sp)[0] = 3; ((uint64_t*)sp)[1] = ei.phdr_va; /* AT_PHDR */
	sp -= 16; ((uint64_t*)sp)[0] = 25; ((uint64_t*)sp)[1] = at_random; /* AT_RANDOM */

	sp -= 8; *(uint64_t*)sp = 0; /* envp NULL */
	sp -= 8; *(uint64_t*)sp = 0; /* argv NULL */
	sp -= 8; *(uint64_t*)sp = at_execfn; /* argv[0] */
	sp -= 8; *(uint64_t*)sp = 1; /* argc */

	arch_context_init_user(ctx, ei.entry, sp);

	if (old_vm) arch_vm_switch((uint64_t)old_vm->root);
	else arch_vm_switch((uint64_t)kernel_vm_space->root);

	p->ctx = ctx;
	p->fds = fd_table_create();
	memcpy(p->cwd, current->cwd, 256);
	p->umask = current->umask;

	{
		uint64_t f = spin_lock_irq(&sched_lock);
		int pid = alloc_pid();
		if (pid < 0) {
			spin_unlock_irq(&sched_lock, f);
			goto fail;
		}
		p->pid  = pid;
		p->tgid = pid;
		p->pgrp = pid;
		p->next = proctab[0]->next;
		proctab[0]->next = p;
		runqueue_add(p);
		proctab[pid] = p;
		spin_unlock_irq(&sched_lock, f);
	}
	return p->pid;

fail:
	if (p->vm) vm_destroy(p->vm);
	if (p->fds) fd_table_destroy(p->fds);
	if (p->kstack) buddy_free(VIRT_TO_PHYS(p->kstack), kstack_order_val);
	buddy_free(VIRT_TO_PHYS(p), 0);
	return -1;
}

pid_t proc_fork(void)
{
	process_t *parent = task_current();
	arch_save_tls(parent);

	process_t *child = (process_t *)PHYS_TO_VIRT(buddy_alloc(0));
	if (!child) return -1;
	memset(child, 0, sizeof(*child));

	child->ppid      = parent->pid;
	child->pgrp      = parent->pgrp;
	child->sid       = parent->sid;
	child->state     = PROC_READY;
	child->uid  = parent->uid;  child->euid = parent->euid; child->suid = parent->suid;
	child->gid  = parent->gid;  child->egid = parent->egid; child->sgid = parent->sgid;
	child->ngroups = parent->ngroups;
	for (int _gi = 0; _gi < parent->ngroups; _gi++) child->groups[_gi] = parent->groups[_gi];
	child->cap_permitted   = parent->cap_permitted;
	child->cap_effective   = parent->cap_effective;
	child->cap_inheritable = parent->cap_inheritable;
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

	/* expensive ops: vm_copy (TLB flush), fd dup, VMA list — all outside sched_lock */
	child->vm = vm_copy(parent->vm);
	if (!child->vm) goto fail_early;

	child->fds = fd_table_copy(parent->fds);
	if (!child->fds) goto fail_early;
	memcpy(child->cwd, parent->cwd, 256);
	child->umask = parent->umask;

	{
		vma_t **dst = &child->vmas;
		for (vma_t *v = parent->vmas; v; v = v->next) {
			vma_t *nv = kmalloc(sizeof(*nv));
			if (!nv) goto fail_early;
			*nv = *v; nv->next = NULL;
			*dst = nv; dst = &nv->next;
		}
	}

	memcpy(child->sig_actions, parent->sig_actions, sizeof(parent->sig_actions));
	child->sig_mask = parent->sig_mask;

	child->kstack = kstack_alloc();
	if (!child->kstack) goto fail_early;

	{
		uint8_t *ksp = child->kstack + kstack_size_val;
		arch_context_t *cctx = (arch_context_t *)(ksp - sizeof(arch_context_t));
		memcpy(cctx, parent->ctx, sizeof(arch_context_t));
		CTX_SYSCALL_RET(cctx, 0);
		child->ctx = cctx;
	}
	child->tls_base        = parent->tls_base;
	child->ticks_remaining = sched_quantum_val;

	{
		uint64_t f = spin_lock_irq(&sched_lock);
		int pid = alloc_pid();
		if (pid < 0) {
			spin_unlock_irq(&sched_lock, f);
			goto fail_early;
		}
		child->pid  = pid;
		child->tgid = pid;
		child->pgrp = parent->pgrp;
		child->next = proctab[0]->next;
		proctab[0]->next = child;
		runqueue_add(child);
		proctab[pid] = child;
		spin_unlock_irq(&sched_lock, f);
	}
	return child->pid;

fail_early:
	if (child->vm)     vm_destroy(child->vm);
	if (child->fds)    fd_table_destroy(child->fds);
	if (child->kstack) buddy_free(VIRT_TO_PHYS(child->kstack), kstack_order_val);
	for (vma_t *v = child->vmas, *nxt; v; v = nxt) { nxt = v->next; kfree(v); }
	buddy_free(VIRT_TO_PHYS(child), 0);
	return -1;
}

pid_t proc_vfork(void)
{
	process_t *parent = task_current();
	arch_save_tls(parent);

	process_t *child = (process_t *)PHYS_TO_VIRT(buddy_alloc(0));
	if (!child) return -1;
	memset(child, 0, sizeof(*child));

	child->ppid         = parent->pid;
	child->pgrp         = parent->pgrp;
	child->state        = PROC_READY;
	child->uid  = parent->uid;  child->euid = parent->euid; child->suid = parent->suid;
	child->gid  = parent->gid;  child->egid = parent->egid; child->sgid = parent->sgid;
	child->ngroups = parent->ngroups;
	for (int _gi = 0; _gi < parent->ngroups; _gi++) child->groups[_gi] = parent->groups[_gi];
	child->cap_permitted   = parent->cap_permitted;
	child->cap_effective   = parent->cap_effective;
	child->cap_inheritable = parent->cap_inheritable;
	child->vm           = vm_get(parent->vm);
	child->brk          = parent->brk;
	child->mmap_next    = parent->mmap_next;
	child->vfork_parent = parent->pid;
	child->reaped       = 0;
	child->fds          = fd_table_copy(parent->fds);
	memcpy(child->name, parent->name, PROC_NAME_MAX);
	memcpy(child->cwd, parent->cwd, 256);
	child->umask        = parent->umask;

	child->kstack = kstack_alloc();
	if (!child->kstack) {
		buddy_free(VIRT_TO_PHYS(child), 0);
		return -1;
	}

	uint8_t *ksp = child->kstack + kstack_size_val;
	arch_context_t *cctx = (arch_context_t *)(ksp - sizeof(arch_context_t));
	memcpy(cctx, parent->ctx, sizeof(arch_context_t));
	CTX_SYSCALL_RET(cctx, 0);
	child->ctx      = cctx;
	child->tls_base = parent->tls_base;

	uint64_t f = spin_lock_irq(&sched_lock);
	int pid = alloc_pid();
	if (pid < 0) {
		spin_unlock_irq(&sched_lock, f);
		buddy_free(VIRT_TO_PHYS(child->kstack), kstack_order_val);
		buddy_free(VIRT_TO_PHYS(child), 0);
		return -1;
	}
	child->pid  = pid;
	child->tgid = pid;
	child->next = proctab[0]->next;
	proctab[0]->next = child;
	runqueue_add(child);
	proctab[pid] = child;

	if (parent->ctx)
		CTX_SYSCALL_RET(parent->ctx, child->pid);

	parent->state = PROC_SLEEPING;
	need_resched  = 1;

	spin_unlock_irq(&sched_lock, f);
	return child->pid;
}

void proc_vfork_exec_notify(void)
{
	process_t *p = task_current();
	if (p->vfork_parent < 0) return;

	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *pp = (p->vfork_parent < proc_cnt) ? proctab[p->vfork_parent] : NULL;
	if (pp && pp->state == PROC_SLEEPING) {
		pp->state = PROC_READY;
		runqueue_add(pp);
		need_resched = 1;
	}
	p->vfork_parent = -1;
	spin_unlock_irq(&sched_lock, f);
}

pid_t proc_clone_thread(uint64_t newsp, uint64_t tls, uint32_t *ctid)
{
	process_t *parent = task_current();
	process_t *child  = (process_t *)PHYS_TO_VIRT(buddy_alloc(0));
	if (!child) return -1;
	memset(child, 0, sizeof(*child));

	child->ppid         = parent->pid;
	child->pgrp         = parent->pgrp;
	child->state        = PROC_READY;
	child->uid  = parent->uid;  child->euid = parent->euid; child->suid = parent->suid;
	child->gid  = parent->gid;  child->egid = parent->egid; child->sgid = parent->sgid;
	child->ngroups = parent->ngroups;
	for (int _gi = 0; _gi < parent->ngroups; _gi++) child->groups[_gi] = parent->groups[_gi];
	child->cap_permitted   = parent->cap_permitted;
	child->cap_effective   = parent->cap_effective;
	child->cap_inheritable = parent->cap_inheritable;
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

	/* Threads share the fd table — bump the table's refcount */
	child->fds = fd_table_get(parent->fds);

	memcpy(child->cwd, parent->cwd, sizeof(child->cwd));
	child->umask = parent->umask;

	child->kstack = kstack_alloc();
	if (!child->kstack) goto fail;

	uint8_t *ksp = child->kstack + kstack_size_val;
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

	child->tls_base = tls ? tls : parent->tls_base;

	/* Write child TID to ctid if requested */
	if (ctid) {
		uint32_t tid = (uint32_t)child->pid;
		/* we can't use copy_to_user here (no current context switch yet),
		 * but ctid is in the shared VM so direct write works */
		*ctid = tid;
	}

	child->ctx             = cctx;
	child->ticks_remaining = sched_quantum_val;

	uint64_t f = spin_lock_irq(&sched_lock);
	int pid = alloc_pid();
	if (pid < 0) {
		spin_unlock_irq(&sched_lock, f);
		goto fail;
	}
	child->pid  = pid;
	child->tgid = parent->tgid;
	child->next      = proctab[0]->next;
	proctab[0]->next = child;
	runqueue_add(child);
	proctab[pid] = child;

	spin_unlock_irq(&sched_lock, f);
	return child->pid;

fail:
	if (child->vm) vm_destroy(child->vm);
	if (child->kstack) buddy_free(VIRT_TO_PHYS(child->kstack), kstack_order_val);
	buddy_free(VIRT_TO_PHYS(child), 0);
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
	/* nice −20 → 2×quantum, nice +19 → ~half quantum */
	int q = sched_quantum_val - (next->nice * sched_quantum_val) / 40;
	if (q < 1) q = 1;
	next->ticks_remaining = q;

	arch_save_tls(prev);

	if (next->pid != 0 && next->kstack &&
	    *(uint64_t *)next->kstack != KSTACK_CANARY)
		kpanic("kstack overflow detected");

	cpu_context_t *cpu = arch_get_cpu_ctx();
	cpu->current = next;
	/* IMPORTANT: update the syscall kstack pointer */
	cpu->kstack_top = next->kstack + kstack_size_val;

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

	/* Steal vm/fds pointers under lock so we own them for teardown.
	 * Keep state PROC_RUNNING so the scheduler can still preempt and
	 * reschedule us during cleanup — prevents use-after-free of kstack. */
	vm_space_t *dying_vm  = p->vm;   p->vm  = NULL;
	fd_table_t *dying_fds = p->fds;  p->fds = NULL;

	spin_unlock_irq(&sched_lock, f);

	/* Free resources — process is still schedulable during this. */
	if (dying_fds) fd_table_put(dying_fds);
	if (dying_vm) {
		vm_activate();
		vm_destroy(dying_vm);
	}
	vma_t *v = p->vmas; p->vmas = NULL;
	while (v) { vma_t *nx = v->next; kfree(v); v = nx; }

	/* Cleanup done — now mark dead and wake parent. */
	f = spin_lock_irq(&sched_lock);
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

	/* Reparent orphaned children to init (pid 1) */
	process_t *init_proc = (proc_cnt > 1 && proctab[1]) ? proctab[1] : NULL;
	process_t *sentinel2 = proctab[0];
	for (process_t *c = sentinel2->next; c && c != sentinel2; c = c->next) {
		if (c->ppid == (pid_t)p->pid && c != p) {
			c->ppid = init_proc ? (pid_t)init_proc->pid : 0;
			if (c->state == PROC_DEAD && init_proc)
				init_proc->sig_pending |= (1UL << (SIGCHLD - 1));
		}
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
		p->sleeping_on_queue = 0;
		runqueue_add(p);
		need_resched = 1;
	}
	spin_unlock_irq(&sched_lock, f);
}

void task_wake_no_resched(process_t *p)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	if (p && p->state == PROC_SLEEPING) {
		p->state = PROC_READY;
		p->sleeping_on_queue = 0;
		runqueue_add(p);
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

void task_signal_pgrp(pid_t pgrp, int sig)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *sentinel = proctab[0];
	for (process_t *t = sentinel->next; t && t != sentinel; t = t->next) {
		if (t->pgrp == pgrp && t->state != PROC_DEAD)
			t->sig_pending |= (1UL << (sig - 1));
	}
	spin_unlock_irq(&sched_lock, f);
}

/* Signals that kill the process by default */
static int sig_default_kill(int sig)
{
	switch (sig) {
	case 1:  /* SIGHUP  */
	case 2:  /* SIGINT  */
	case 3:  /* SIGQUIT */
	case 6:  /* SIGABRT */
	case 8:  /* SIGFPE  */
	case 9:  /* SIGKILL */
	case 11: /* SIGSEGV */
	case 13: /* SIGPIPE */
	case 14: /* SIGALRM */
	case 15: /* SIGTERM */
		return 1;
	default:
		return 0;
	}
}

void signal_dispatch(arch_context_t *tf)
{
	/* Only deliver signals when returning to user space */
#if defined(__x86_64__)
	if ((tf->cs & 3) != 3) return;
#elif defined(__aarch64__)
	if ((tf->spsr & 0xF) != 0) return;
#endif
	process_t *p = task_current();
	if (!p) return;

	uint64_t mask = p->sig_pending & ~p->sig_mask;
	if (!mask) return;

	int sig = 0;
	for (int i = 0; i < NSIG; i++) {
		if (mask & (1UL << i)) { sig = i + 1; break; }
	}
	if (!sig) return;

	p->sig_pending &= ~(1UL << (sig - 1));

	sigaction_t *sa = &p->sig_actions[sig - 1];
	void (*handler)(int) = sa->sa_handler;

	/* SIG_IGN = 1 */
	if ((uintptr_t)handler == 1) return;

	/* SIG_DFL = 0 or NULL */
	if (!handler) {
		if (sig_default_kill(sig)) {
			p->exit_code      = sig;
			p->killed_by_sig  = 1;
			task_exit();
		}
		return;
	}

	/* Deliver to user handler: redirect the trap frame */
	/* Block this signal during handler execution; save old mask for restore */
	uint64_t old_mask = p->sig_mask;
	p->sig_mask |= (1UL << (sig - 1));
	p->sig_mask |= sa->sa_mask;

#if defined(__aarch64__)
	/* Frame layout — 256 bytes, 16-byte aligned:
	 *   [sp+  0]  orig_elr  (PC to resume)
	 *   [sp+  8]  orig_x30  (LR)
	 *   [sp+ 16]  old_mask
	 *   [sp+ 24]  padding
	 *   [sp+ 32]  orig_x0 .. orig_x18  (19 caller-saved + indirect-result regs)
	 *   [sp+184]  orig_x29 (frame pointer)
	 *   [sp+192]  orig_spsr
	 *   [sp+200]  padding × 7  (→ total 256)
	 */
	uint64_t usp = (tf->sp - 256) & ~15UL;
	*(uint64_t *)(usp +   0) = tf->elr;
	*(uint64_t *)(usp +   8) = tf->regs[30];
	*(uint64_t *)(usp +  16) = old_mask;
	*(uint64_t *)(usp +  24) = 0;
	for (int _i = 0; _i <= 18; _i++)
		*(uint64_t *)(usp + 32 + (uint64_t)_i * 8) = tf->regs[_i];
	*(uint64_t *)(usp + 184) = tf->regs[29];
	*(uint64_t *)(usp + 192) = tf->spsr;
	tf->sp                   = usp;
	tf->regs[30]             = sa->sa_restorer ? (uint64_t)sa->sa_restorer
	                                            : tf->elr;
	tf->regs[0]              = (uint64_t)sig;
	tf->elr                  = (uint64_t)handler;
#elif defined(__x86_64__)
	/* Frame layout — 168 bytes, rsp kept 8 mod 16 (post-call ABI convention):
	 *   [rsp+  0]  return addr (sa_restorer → rt_sigreturn)
	 *   [rsp+  8]  old_mask
	 *   [rsp+ 16]  orig_rip
	 *   [rsp+ 24]  orig_rsp
	 *   [rsp+ 32]  orig_rflags
	 *   [rsp+ 40]  orig_rax
	 *   [rsp+ 48]  orig_rbx
	 *   [rsp+ 56]  orig_rcx
	 *   [rsp+ 64]  orig_rdx
	 *   [rsp+ 72]  orig_rsi
	 *   [rsp+ 80]  orig_rdi
	 *   [rsp+ 88]  orig_rbp
	 *   [rsp+ 96]  orig_r8
	 *   [rsp+104]  orig_r9
	 *   [rsp+112]  orig_r10
	 *   [rsp+120]  orig_r11
	 *   [rsp+128]  orig_r12
	 *   [rsp+136]  orig_r13
	 *   [rsp+144]  orig_r14
	 *   [rsp+152]  orig_r15
	 *   [rsp+160]  padding × 1  (→ total 168, rsp % 16 == 8)
	 * rt_sigreturn reads base = ctx->rsp (= frame_base+8 after handler ret). */
	uint64_t usp = (tf->rsp & ~15UL) - 168; /* frame below RSP, 8-mod-16 for handler entry */
	*(uint64_t *)(usp +   0) = sa->sa_restorer ? (uint64_t)sa->sa_restorer : tf->rip;
	*(uint64_t *)(usp +   8) = old_mask;
	*(uint64_t *)(usp +  16) = tf->rip;
	*(uint64_t *)(usp +  24) = tf->rsp;
	*(uint64_t *)(usp +  32) = tf->rflags;
	*(uint64_t *)(usp +  40) = tf->rax;
	*(uint64_t *)(usp +  48) = tf->rbx;
	*(uint64_t *)(usp +  56) = tf->rcx;
	*(uint64_t *)(usp +  64) = tf->rdx;
	*(uint64_t *)(usp +  72) = tf->rsi;
	*(uint64_t *)(usp +  80) = tf->rdi;
	*(uint64_t *)(usp +  88) = tf->rbp;
	*(uint64_t *)(usp +  96) = tf->r8;
	*(uint64_t *)(usp + 104) = tf->r9;
	*(uint64_t *)(usp + 112) = tf->r10;
	*(uint64_t *)(usp + 120) = tf->r11;
	*(uint64_t *)(usp + 128) = tf->r12;
	*(uint64_t *)(usp + 136) = tf->r13;
	*(uint64_t *)(usp + 144) = tf->r14;
	*(uint64_t *)(usp + 152) = tf->r15;
	*(uint64_t *)(usp + 160) = 0;
	tf->rsp                  = usp;
	tf->rdi                  = (uint64_t)sig;
	tf->rip                  = (uint64_t)handler;
#endif
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

void proc_find_zombie(pid_t ppid, process_t **out_child, int *out_any_alive)
{
	*out_child = NULL;
	*out_any_alive = 0;
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *sentinel = proctab[0];
	for (process_t *c = sentinel->next; c && c != sentinel; c = c->next) {
		if (c->ppid == ppid) {
			if (c->state == PROC_DEAD && !c->reaped) {
				c->reaped = 1;
				*out_child = c;
				break;
			}
			if (c->state != PROC_DEAD)
				*out_any_alive = 1;
		}
	}
	spin_unlock_irq(&sched_lock, f);
}

void proc_reap(process_t *child)
{
	pid_t cpid = child->pid;
	uint8_t *kstack = child->kstack;

	uint64_t f = spin_lock_irq(&sched_lock);
	if (cpid >= 0 && cpid < proc_cnt)
		proctab[cpid] = NULL;
		
	/* remove from the global process linked list to prevent Use-After-Free */
	process_t **p = &proctab[0]->next;
	while (*p && *p != child) {
		p = &(*p)->next;
	}
	if (*p == child) {
		*p = child->next;
	}
	spin_unlock_irq(&sched_lock, f);

	if (kstack)
		buddy_free(VIRT_TO_PHYS(kstack), kstack_order_val);
	buddy_free(VIRT_TO_PHYS(child), 0);
}
