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
#include <kernel/smp.h>
#include <asm/mmu.h>

extern void futex_wake_addr(uint32_t *uaddr);

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

/* Rotating pid allocation (Linux-style last_pid): search forward from the
 * last allocated slot so a freed pid/tid is not reused immediately. Quick
 * reuse can collide with stale userspace state keyed on tid (e.g. musl's
 * __thread_list_lock holds the owner's tid). */
static int last_pid = 0;
static int alloc_pid(void)
{
	for (int n = 1; n < proc_cnt; n++) {
		int i = last_pid + n;
		if (i >= proc_cnt) i -= (proc_cnt - 1);   /* wrap, skipping slot 0 */
		if (i == 0) i = 1;
		if (!proctab[i]) { last_pid = i; return i; }
	}
	if (proctab_ensure() != 0) return -1;
	last_pid = proc_cnt;
	return proc_cnt++;
}

#define KSTACK_CANARY 0xDEADBEEFCAFEBABEULL

static int kstack_size_val  = KSTACK_SIZE;
static int kstack_order_val = 2; /* buddy order; 2 = 4 pages = 16384 bytes */

static uint8_t *kstack_alloc(void)
{
	void *p = buddy_alloc(kstack_order_val);
	if (!p) return NULL;
	uint8_t *ks = (uint8_t *)PHYS_TO_VIRT(p);
	*(uint64_t *)ks = KSTACK_CANARY;
	return ks;
}

spinlock_t sched_lock = SPINLOCK_INIT;
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

/* Runs on CPU0 only. A CPU whose wd_ticks froze since the last sweep took no
 * timer interrupt for ~2.5s — wedged with IRQs masked (spinlock deadlock or a
 * stuck IRQ-off section). Report which CPU, where, and on what. */
static void sched_watchdog(void)
{
	extern int smp_ncpus;
	extern cpu_context_t *all_cpus[];
	static uint64_t last_seen[MAX_CPUS];
	static unsigned n;
	static int warm;

	if ((++n & 0xFF) != 0) return;   /* ~every 256 ticks (2.56s @100Hz) */

	if (warm < 2) {
		warm++;
		for (int i = 0; i < smp_ncpus; i++)
			if (all_cpus[i]) last_seen[i] = all_cpus[i]->wd_ticks;
		return;
	}

	for (int i = 0; i < smp_ncpus; i++) {
		cpu_context_t *c = all_cpus[i];
		if (!c || !c->online) continue;
		process_t *cur = c->current;
		/* An idle CPU halted in WFI/HLT legitimately takes no tick until an IPI
		 * wakes it (x86 secondaries lack a per-CPU timer) — only a CPU running
		 * real work with frozen ticks is wedged. */
		if (cur && cur->pid != 0 && c->wd_ticks == last_seen[i]) {
			kprintf("WATCHDOG: cpu%d wedged, where=%s pid=%d (%s)\n",
			        i, c->wd_where ? c->wd_where : "?",
			        (int)cur->pid, cur->name);
			kpanic("cpu wedged (watchdog)");
		}
		last_seen[i] = c->wd_ticks;
	}
}

void tick_notify(void)
{
	/* sched_tick always runs on every CPU */
	sched_tick();
	/* Global callbacks (tcp_tick, timerfd_tick) run on CPU0 only */
	if (arch_get_cpu_ctx()->cpu_id == 0) {
		for (int i = 1; i < tick_cb_cnt; i++)
			tick_cbs[i]();
		sched_watchdog();
	}
}

void tick_notify_local(void)
{
	sched_tick();
}

volatile uint64_t kernel_ticks  = 0;

/* Per-CPU runqueue helpers — caller must hold sched_lock */

static void cpu_rq_add(cpu_context_t *cpu, process_t *p)
{
	if ((uint64_t)p < 0xFFFF000000000000ULL) {
		kprintf("[DBG] cpu_rq_add: bad p=%p cpu%d\n", p, (int)cpu->cpu_id);
		kpanic("cpu_rq_add: invalid process ptr");
	}
	p->sched_next = NULL;
	if (!cpu->rq_tail) {
		cpu->rq_head = cpu->rq_tail = p;
	} else {
		cpu->rq_tail->sched_next = p;
		cpu->rq_tail = p;
	}
}

static process_t *cpu_rq_pop(cpu_context_t *cpu)
{
	if (!cpu->rq_head) return NULL;
	process_t *p = cpu->rq_head;
	if ((uint64_t)p < 0xFFFF000000000000ULL) {
		kprintf("[DBG] cpu_rq_pop: bad head %p on cpu%d rq_tail=%p\n",
		        p, (int)cpu->cpu_id, cpu->rq_tail);
		cpu->rq_head = cpu->rq_tail = NULL;
		kpanic("cpu_rq_pop: corrupt rq_head");
	}
	process_t *nxt = p->sched_next;
	if (nxt && (uint64_t)nxt < 0xFFFF000000000000ULL) {
		kprintf("[DBG] cpu_rq_pop: bad sched_next %p in proc pid=%lld cpu%d\n",
		        nxt, (long long)p->pid, (int)cpu->cpu_id);
		kpanic("cpu_rq_pop: corrupt sched_next");
	}
	cpu->rq_head = nxt;
	if (!cpu->rq_head) cpu->rq_tail = NULL;
	p->sched_next = NULL;
	return p;
}

/* Count processes in a CPU's runqueue */
static int cpu_rq_len(cpu_context_t *cpu)
{
	int n = 0;
	for (process_t *p = cpu->rq_head; p; p = p->sched_next) n++;
	return n;
}

/* Pick the least-loaded online CPU */
static cpu_context_t *pick_cpu(void)
{
#ifdef __aarch64__
	extern int smp_ncpus;
	extern cpu_context_t *all_cpus[];
	cpu_context_t *best = arch_get_cpu_ctx();
	int best_len = cpu_rq_len(best);
	for (int i = 0; i < smp_ncpus; i++) {
		cpu_context_t *c = all_cpus[i];
		if (!c || !c->online) continue;
		int l = cpu_rq_len(c);
		if (l < best_len) { best = c; best_len = l; }
	}
	return best;
#else
	return arch_get_cpu_ctx();
#endif
}

/* Internal: must hold sched_lock */
static void runqueue_add(process_t *p)
{
	if (p->pid == 0) return;
	cpu_context_t *cpu = pick_cpu();
	cpu_rq_add(cpu, p);
	cpu->need_resched = 1;
	if (cpu != arch_get_cpu_ctx()) {
		extern void smp_ipi_resched(int);
		int cid = (int)cpu->cpu_id;
		smp_ipi_resched(cid);
		__asm__ volatile("" ::: "memory");
	}
}

/* Pop from current CPU; if empty, try work-stealing */
static process_t *runqueue_pop(void)
{
	cpu_context_t *cpu = arch_get_cpu_ctx();
	process_t *p = cpu_rq_pop(cpu);
	if (p) return p;

	/* work stealing: find busiest CPU */
	extern int smp_ncpus;
	extern cpu_context_t *all_cpus[];
	cpu_context_t *victim = NULL;
	int max_len = 1;  /* only steal if > 1 task */
	for (int i = 0; i < smp_ncpus; i++) {
		cpu_context_t *c = all_cpus[i];
		if (!c || !c->online || c == cpu) continue;
		int l = cpu_rq_len(c);
		if (l > max_len) { victim = c; max_len = l; }
	}
	if (!victim) return NULL;
	return cpu_rq_pop(victim);
}



/* ── scheduler ───────────────────────────────────────────── */

static void idle_loop(void)
{
	/* Reschedule the instant a task becomes runnable instead of sleeping in
	 * WFI/HLT until the next timer tick — yield/wake latency 10ms→µs. No lost
	 * wakeups: need_resched on an idle CPU is set by an interrupt (wakes WFI)
	 * or the requeue just before idling (caught on the first iteration). */
	while (1) {
		cpu_context_t *c = arch_get_cpu_ctx();
		if (c->need_resched) {
			c->need_resched = 0;   /* consume the hint; runqueue is the truth */
			arch_yield();
		} else {
			arch_halt();
		}
	}
}

void sched_init(void)
{
	process_t *p = &idle_proc;
	memset(p, 0, sizeof(*p));
	p->pid   = 0;
	p->state = PROC_RUNNING;
	p->on_cpu = 1;
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
	cpu->self         = cpu;
	cpu->current      = p;
	cpu->kstack_top   = sp_top;
	cpu->irq_stack    = irq_stack + KSTACK_SIZE;
	cpu->cpu_id       = 0;
	cpu->magic        = 0xCAFEBABE;
	cpu->rq_head      = NULL;
	cpu->rq_tail      = NULL;
	cpu->online       = 1;
	cpu->need_resched = 0;
	cpu->idle         = p;
	all_cpus[0]       = cpu;

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
	arch_get_cpu_ctx()->wd_ticks++;   /* watchdog heartbeat (timer-IRQ driven) */
	process_t *current = task_current();
	if (!current) return;

	current->ticks++;

	uint64_t f = spin_lock_irq(&sched_lock);

	if (current->pid != 0) {
		if (--current->ticks_remaining <= 0)
			arch_get_cpu_ctx()->need_resched = 1;
	} else if (arch_get_cpu_ctx()->rq_head) {
		arch_get_cpu_ctx()->need_resched = 1;
	}

	/* Walk process list: fire interval timers and wake nanosleeping tasks */
	process_t *sentinel = proctab[0];
	for (process_t *t = sentinel->next; t && t != sentinel; t = t->next) {
		if (t->itimer_value > 0) {
			t->itimer_value--;
			if (t->itimer_value == 0) {
				t->itimer_value = t->itimer_interval;
				t->sig_pending |= (1ULL << (14 - 1));
				if (t->state == PROC_SLEEPING) {
					t->state = PROC_READY;
					if (!t->on_cpu) runqueue_add(t);
					arch_get_cpu_ctx()->need_resched = 1;
				}
			}
		}
		if (t->state == PROC_SLEEPING && !t->sleeping_on_queue) {
			t->state = PROC_READY;
			if (!t->on_cpu) runqueue_add(t);
			arch_get_cpu_ctx()->need_resched = 1;
		}
	}

	spin_unlock_irq(&sched_lock, f);
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
	arch_get_cpu_ctx()->need_resched = 1;

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
		if (!pp->on_cpu) runqueue_add(pp);
		arch_get_cpu_ctx()->need_resched = 1;
	}
	p->vfork_parent = -1;
	spin_unlock_irq(&sched_lock, f);
}

pid_t proc_clone_thread(uint64_t newsp, uint64_t tls, uint32_t *ctid, uint32_t *ptid)
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

	child->clear_child_tid = ctid;

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
	proctab[pid] = child;
	/* Write real child TID to ctid/ptid before child is runnable.
	 * ptid (CLONE_PARENT_SETTID) is required: musl's __tl_lock keys on
	 * pthread_self()->tid, so a 0 tid silently disables thread-list
	 * locking and corrupts the list under SMP. */
	if (ctid) *ctid = (uint32_t)pid;
	if (ptid) *ptid = (uint32_t)pid;
	/* Publish the tid (and all child setup) to memory before the child can
	 * be observed runnable on another CPU. On weakly-ordered aarch64 a stale
	 * tid=0 read makes musl's __tl_lock a no-op and corrupts the thread list. */
	smp_mb();
	runqueue_add(child);

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
		if (!p || p->state == PROC_DEAD || p->state == PROC_ZOMBIE) {
			spin_unlock_irq(&sched_lock, f);
			break;
		}
		spin_unlock_irq(&sched_lock, f);
		task_sleep();
	}
}

/*
 * Called from assembly after the stack switch (mov sp, x0) in irq_handler and
 * .yield.  At that point we are on the NEW process's kstack, so it is safe to
 * release resources belonging to the process we just switched away from.
 */
void sched_free_dying_kstack(void)
{
	cpu_context_t *cpu = arch_get_cpu_ctx();

	if (cpu->dying_kstack) {
		buddy_free(VIRT_TO_PHYS(cpu->dying_kstack), cpu->dying_kstack_order);
		cpu->dying_kstack = NULL;
	}
}

/*
 * Called from assembly AFTER the stack switch (mov sp, x0).  At that point
 * the previous process's kstack is no longer in use by this CPU, so it is
 * safe to make prev visible to other CPUs via the runqueue.
 */
void sched_requeue_prev(void)
{
	cpu_context_t *cpu = arch_get_cpu_ctx();
	process_t *p = cpu->pending_requeue;
	if (!p) return;
	cpu->pending_requeue = NULL;

	uint64_t f = spin_lock_irq(&sched_lock);
	p->on_cpu = 0;
	/* Only requeue a genuinely runnable process. A process that exited
	 * after being marked for requeue (PROC_DEAD/ZOMBIE) must not go back
	 * into the runqueue, or schedule() could resume its task_exit. */
	if (p->state == PROC_READY)
		runqueue_add(p);
	spin_unlock_irq(&sched_lock, f);
}

void schedule(void)
{
	arch_get_cpu_ctx()->wd_where = "schedule";
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *prev = task_current();

	cpu_context_t *cpu = arch_get_cpu_ctx();
	
	/* Always pass prev to sched_requeue_prev to clear on_cpu */
	cpu->pending_requeue = prev;

	if (prev->state == PROC_RUNNING) {
		prev->state = PROC_READY;
	}

	process_t *next = runqueue_pop();
	/* Never run a process that has already exited. A dying process can be
	 * left in a runqueue (e.g. requeued READY in the return path, then it
	 * takes a fatal signal and calls task_exit before being descheduled).
	 * Running it would resume task_exit's arch_yield, making task_exit
	 * "return" and the fatal signal re-fire — an unkillable livelock. */
	while (next && (next->state == PROC_DEAD || next->state == PROC_ZOMBIE)) {
		next->on_cpu = 0;
		next = runqueue_pop();
	}
	if (!next) {
		cpu_context_t *_cpu = arch_get_cpu_ctx();
		next = _cpu->idle ? _cpu->idle : &idle_proc;
	}
	if ((uint64_t)next < 0xFFFF000000000000ULL) {
		kprintf("[DBG] schedule: bad next=%p prev=%p cpu%d rq_head=%p rq_tail=%p\n",
		        next, prev, (int)arch_get_cpu_ctx()->cpu_id,
		        arch_get_cpu_ctx()->rq_head, arch_get_cpu_ctx()->rq_tail);
		kpanic("schedule: invalid next process");
	}
	next->state = PROC_RUNNING;
	next->on_cpu = 1;
	/* nice −20 → 2×quantum, nice +19 → ~half quantum */
	int q = sched_quantum_val - (next->nice * sched_quantum_val) / 40;
	if (q < 1) q = 1;
	next->ticks_remaining = q;

	arch_save_tls(prev);

	if (next->pid != 0 && next->kstack &&
	    *(uint64_t *)next->kstack != KSTACK_CANARY)
		kpanic("kstack overflow detected");

	/*
	 * When switching away from a dead process: defer kstack freeing until
	 * after the stack switch (mov sp,x0), then mark it PROC_ZOMBIE so
	 * proc_reap knows schedule() has completed and process_t is safe to free.
	 * Wake the parent again — it may have found PROC_DEAD too early.
	 */
	if (prev->state == PROC_DEAD && prev->pid != 0 && prev->kstack) {
		cpu->dying_kstack       = prev->kstack;
		cpu->dying_kstack_order = kstack_order_val;
		prev->kstack = NULL;
		prev->state  = PROC_ZOMBIE;
		if (prev->ppid > 0 && (int)prev->ppid < proc_cnt) {
			process_t *pp = proctab[prev->ppid];
			if (pp && pp->state == PROC_SLEEPING) {
				pp->state = PROC_READY;
				if (!pp->on_cpu) runqueue_add(pp);
			}
		}
	}

	cpu->current = next;
	/* IMPORTANT: update the syscall kstack pointer */
	cpu->kstack_top = next->kstack + kstack_size_val;
	smp_mb();

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
	process_t *p = task_current();
	/* Re-entry guard: if this process is already exiting (e.g. a second
	 * fatal signal, or it was resumed from a runqueue while dying), do not
	 * run the teardown again — just leave the CPU and never return. */
	if (p->state == PROC_DEAD || p->state == PROC_ZOMBIE) {
		for (;;) {
			uint64_t gf = spin_lock_irq(&sched_lock);
			p->state = PROC_DEAD;
			spin_unlock_irq(&sched_lock, gf);
			arch_yield();
		}
	}
	uint32_t *cctid = p->clear_child_tid;

	/* Write tid=0 and wake futex waiter while p->vm is still set.
	 * Must happen before stealing p->vm: a preemption between p->vm=NULL
	 * and this write would cause vm_activate to load the kernel PT, making
	 * the user-address write fault at EL1. futex_wake_addr must also be
	 * called outside sched_lock to preserve lock order (futex→sched). */
	if (cctid) {
		*cctid = 0;
		futex_wake_addr(cctid);
	}

	uint64_t f = spin_lock_irq(&sched_lock);
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
			if (!pp->on_cpu) runqueue_add(pp);
			arch_get_cpu_ctx()->need_resched = 1;
		}
		p->vfork_parent = -1;
	}

	/* Reparent orphaned children to init (pid 1) */
	process_t *init_proc = (proc_cnt > 1 && proctab[1]) ? proctab[1] : NULL;
	process_t *sentinel2 = proctab[0];
	for (process_t *c = sentinel2->next; c && c != sentinel2; c = c->next) {
		if (c->ppid == (pid_t)p->pid && c != p) {
			c->ppid = init_proc ? (pid_t)init_proc->pid : 0;
			if ((c->state == PROC_DEAD || c->state == PROC_ZOMBIE) && init_proc)
				init_proc->sig_pending |= (1UL << (SIGCHLD - 1));
		}
	}

	process_t *parent = (p->ppid < proc_cnt) ? proctab[p->ppid] : NULL;
	if (parent) {
		if (parent->state == PROC_SLEEPING) {
			parent->state = PROC_READY;
			if (!parent->on_cpu) runqueue_add(parent);
			arch_get_cpu_ctx()->need_resched = 1;
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
		if (!p->on_cpu) runqueue_add(p);
		arch_get_cpu_ctx()->need_resched = 1;
	}
	spin_unlock_irq(&sched_lock, f);
}

void task_wake_no_resched(process_t *p)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	if (p && p->state == PROC_SLEEPING) {
		p->state = PROC_READY;
		p->sleeping_on_queue = 0;
		if (!p->on_cpu) runqueue_add(p);
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
		if (!p->on_cpu) runqueue_add(p);
		arch_get_cpu_ctx()->need_resched = 1;
	}
	spin_unlock_irq(&sched_lock, f);
}

void task_signal_pgrp(pid_t pgrp, int sig)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *sentinel = proctab[0];
	for (process_t *t = sentinel->next; t && t != sentinel; t = t->next) {
		if (t->pgrp == pgrp && t->state != PROC_DEAD && t->state != PROC_ZOMBIE)
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

int task_has_actionable_signal(void)
{
	process_t *p = task_current();
	uint64_t mask = p->sig_pending & ~p->sig_mask;
	while (mask) {
		int sig = __builtin_ctzll(mask) + 1;
		mask &= mask - 1;
		void (*handler)(int) = p->sig_actions[sig - 1].sa_handler;
		if ((uintptr_t)handler == 1) continue;   /* SIG_IGN */
		if (!handler && !sig_default_kill(sig)) continue; /* SIG_DFL non-fatal */
		return 1;
	}
	return 0;
}

void signal_dispatch(arch_context_t *tf)
{
	arch_get_cpu_ctx()->wd_where = "signal_dispatch";
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
	/* Frame layout — 736 bytes, 16-byte aligned:
	 *   [sp+  0]  orig_elr  (PC to resume)
	 *   [sp+  8]  orig_x30  (LR)
	 *   [sp+ 16]  old_mask
	 *   [sp+ 24]  padding
	 *   [sp+ 32]  orig_x0 .. orig_x18  (19 caller-saved + indirect-result regs)
	 *   [sp+184]  orig_x29 (frame pointer)
	 *   [sp+192]  orig_spsr
	 *   [sp+200]  orig_fpsr (4) + orig_fpcr (4)
	 *   [sp+208]  orig_q0 .. q31  (512 bytes)
	 *   [sp+720]  padding × 2  (→ total 736)
	 *
	 * The FP/SIMD state MUST round-trip: musl's memcpy/strlen are vectorised,
	 * so a signal landing mid-copy whose handler clobbers q-regs would resume
	 * the interrupted op with garbage SIMD state and corrupt memory silently.
	 */
	uint64_t usp = (tf->sp - 736) & ~15UL;
	*(uint64_t *)(usp +   0) = tf->elr;
	*(uint64_t *)(usp +   8) = tf->regs[30];
	*(uint64_t *)(usp +  16) = old_mask;
	*(uint64_t *)(usp +  24) = 0;
	for (int _i = 0; _i <= 18; _i++)
		*(uint64_t *)(usp + 32 + (uint64_t)_i * 8) = tf->regs[_i];
	*(uint64_t *)(usp + 184) = tf->regs[29];
	*(uint64_t *)(usp + 192) = tf->spsr;
	*(uint32_t *)(usp + 200) = tf->fpsr;
	*(uint32_t *)(usp + 204) = tf->fpcr;
	for (int _i = 0; _i < 64; _i++)
		*(uint64_t *)(usp + 208 + (uint64_t)_i * 8) = tf->qregs[_i];
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
	 *   [rsp+160]  padding × 1
	 *   [rsp+168]  fxsave area (512 bytes, 16-byte aligned)  (→ total 680)
	 * rt_sigreturn reads base = ctx->rsp (= frame_base+8 after handler ret).
	 * The FP/SSE state must round-trip or a handler's xmm use corrupts the
	 * interrupted code's vector registers (same class as the aarch64 bug). */
	uint64_t usp = (tf->rsp & ~15UL) - 680; /* 8-mod-16 entry; usp+168 is 16-aligned */
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
	memcpy((void *)(usp + 168), tf->fxsave, sizeof(tf->fxsave));  /* 16-aligned */
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
			if (c->state == PROC_ZOMBIE && !c->reaped) {
				c->reaped = 1;
				*out_child = c;
				break;
			}
			if (c->state != PROC_ZOMBIE)
				*out_any_alive = 1;
		}
	}
	spin_unlock_irq(&sched_lock, f);
}

/* Like proc_find_zombie but, if no zombie is found and will_sleep is set,
 * atomically marks parent PROC_SLEEPING under sched_lock so that a
 * concurrent task_exit cannot sneak its wakeup between our check and sleep. */
void proc_find_zombie_or_sleep(pid_t ppid, process_t *parent, int will_sleep,
                               process_t **out_child, int *out_any_alive)
{
	*out_child = NULL;
	*out_any_alive = 0;
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *sentinel = proctab[0];
	for (process_t *c = sentinel->next; c && c != sentinel; c = c->next) {
		if (c->ppid == ppid) {
			if (c->state == PROC_ZOMBIE && !c->reaped) {
				c->reaped = 1;
				*out_child = c;
				break;
			}
			if (c->state != PROC_ZOMBIE)
				*out_any_alive = 1;
		}
	}
	if (!*out_child && *out_any_alive && will_sleep)
		parent->state = PROC_SLEEPING;
	spin_unlock_irq(&sched_lock, f);
}

/* Atomically check child state: zombie=1, ptrace_stop=2, else sleep (0).
 * If sleeping and will_sleep is set, marks parent PROC_SLEEPING under the
 * same lock hold that checked the child — no wakeup can slip through. */
int proc_check_or_sleep(process_t *child, process_t *parent, int will_sleep)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	int zombie = (child->state == PROC_ZOMBIE);
	int ptrace = !zombie && child->ptrace_stop && !child->ptrace_stop_reported;
	if (ptrace)
		child->ptrace_stop_reported = 1;
	if (!zombie && !ptrace && will_sleep)
		parent->state = PROC_SLEEPING;
	spin_unlock_irq(&sched_lock, f);
	return zombie ? 1 : (ptrace ? 2 : 0);
}

void proc_reap(process_t *child)
{
	pid_t cpid = child->pid;

	uint64_t f = spin_lock_irq(&sched_lock);
	if (cpid >= 0 && cpid < proc_cnt)
		proctab[cpid] = NULL;

	process_t **p = &proctab[0]->next;
	while (*p && *p != child)
		p = &(*p)->next;
	if (*p == child)
		*p = child->next;
	spin_unlock_irq(&sched_lock, f);

	/*
	 * kstack was already freed by sched_free_dying_kstack() (called from
	 * assembly after the stack switch), and schedule() cleared child->kstack.
	 * proc_find_zombie only returns PROC_ZOMBIE processes, meaning schedule()
	 * has completed — no CPU has child as cpu->current anymore.
	 */
	if (child->kstack)
		buddy_free(VIRT_TO_PHYS(child->kstack), kstack_order_val);
	buddy_free(VIRT_TO_PHYS(child), 0);
}
