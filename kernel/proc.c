/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* ── process lifecycle ────────────────────────────────────────────────────────
 * Creating processes (proc_create / proc_exec for kernel tasks, proc_fork /
 * proc_clone_thread for the clone family), tearing them down
 * (task_exit), and reaping (proc_find_zombie, the wait helpers, proc_reap).
 * Looking up a process by pid lives here too. The scheduling mechanics these
 * lean on — runqueues, alloc_pid, schedule() — stay in sched.c.
 *
 * Compiled as part of sched.c (textual include): the scheduler statics
 * (proctab, sched_lock, alloc_pid, runqueue_add, kstack_order_val) are in scope.
 * ───────────────────────────────────────────────────────────────────────── */

_Static_assert(sizeof(process_t) <= (PAGE_SIZE << PCB_ORDER),
               "process_t exceeds its PCB allocation (raise PCB_ORDER)");

static spinlock_t sighand_lock;

/* affinity masks are owned per-PCB: fork/clone duplicate, teardown frees. A
 * NULL mask means unrestricted, so an allocation failure degrades to that. */
static struct cpumask *cpu_affinity_dup(const struct cpumask *src)
{
	if (!src) return NULL;
	struct cpumask *m = kmalloc(sizeof(*m));
	if (m) *m = *src;
	return m;
}

static void cpu_affinity_free(process_t *p)
{
	if (p->cpu_affinity) { kfree(p->cpu_affinity); p->cpu_affinity = NULL; }
}

static void proc_cmdline_free(process_t *p)
{
	if (p->cmdline) { kfree(p->cmdline); p->cmdline = NULL; p->cmdline_len = 0; }
}

#define PROC_CMDLINE_MAX 4096   /* Linux caps /proc/<pid>/cmdline at the arg page */

void proc_set_cmdline(process_t *p, const char *block, uint32_t len)
{
	proc_cmdline_free(p);
	if (!block || !len) return;
	if (len > PROC_CMDLINE_MAX) len = PROC_CMDLINE_MAX;
	char *copy = kmalloc(len);
	if (!copy) return;
	memcpy(copy, block, len);
	p->cmdline = copy;
	p->cmdline_len = len;
}

sighand_t *sighand_alloc(sighand_t *copy_from)
{
	sighand_t *sh = kmalloc(sizeof(*sh));
	if (!sh) return NULL;
	if (copy_from) memcpy(sh->actions, copy_from->actions, sizeof(sh->actions));
	else           memset(sh->actions, 0, sizeof(sh->actions));
	sh->refcount = 1;
	return sh;
}

sighand_t *sighand_get(sighand_t *sh)
{
	if (sh) {
		uint64_t f = spin_lock_irq(&sighand_lock);
		sh->refcount++;
		spin_unlock_irq(&sighand_lock, f);
	}
	return sh;
}

void sighand_put(sighand_t *sh)
{
	if (!sh) return;
	uint64_t f = spin_lock_irq(&sighand_lock);
	int r = --sh->refcount;
	spin_unlock_irq(&sighand_lock, f);
	if (r == 0) kfree(sh);
}

pid_t proc_create(const char *name, void (*fn)(void))
{
	void *pcb_phys = buddy_alloc(PCB_ORDER);
	if (!pcb_phys) return -1;
	process_t *p = (process_t *)PHYS_TO_VIRT(pcb_phys);
	memset(p, 0, sizeof(*p));
	p->sighand = sighand_alloc(NULL);
	if (!p->sighand) { buddy_free(VIRT_TO_PHYS(p), PCB_ORDER); return -1; }
	p->magic = PROC_MAGIC;
	p->start_tick = kernel_ticks;

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
		buddy_free(VIRT_TO_PHYS(p), PCB_ORDER);
		return -1;
	}

	p->fds = fd_table_create();
	p->fs  = fs_new();

	uint8_t *sp_top = p->kstack + kstack_size_val;
	arch_context_t *ctx = (arch_context_t *)(sp_top - sizeof(arch_context_t));
	arch_context_init(ctx, fn, (uint64_t)sp_top);
	p->ctx = ctx;

	uint64_t f = spin_lock_irq(&sched_lock);
	int pid = alloc_pid();
	if (pid < 0) {
		spin_unlock_irq(&sched_lock, f);
		buddy_free(VIRT_TO_PHYS(p->kstack), kstack_order_val);
		buddy_free(VIRT_TO_PHYS(p), PCB_ORDER);
		return -1;
	}
	p->pid  = pid;
	p->tgid = pid;
	p->pgrp = pid;
	p->next = proctab[0]->next;
	proctab[0]->next = p;
	runqueue_add(p);
	proctab[pid] = p;
	pidns_attach(p, &init_pid_ns, pid);
	spin_unlock_irq(&sched_lock, f);
	return p->pid;
}

/* The first user task's ids: the creator's. suid/sgid become the new
 * euid/egid (no setuid bit here); as root it gets every capability. */
static void init_creds(process_t *p, const process_t *current)
{
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
}

/* name is the image's full path: comm is its basename, exe_path the whole
 * path (glibc readlinks /proc/self/exe and asserts it is absolute). */
static void init_names(process_t *p, const char *name)
{
	const char *base = name;
	for (const char *s = name; *s; s++) if (*s == '/') base = s + 1;
	int nlen = 0;
	while (base[nlen] && nlen < PROC_NAME_MAX - 1) nlen++;
	memcpy(p->name, base, nlen);
	p->name[nlen] = '\0';
	if (name[0] == '/') {
		int el = 0;
		while (name[el] && el < PROC_NAME_MAX - 1) { p->exe_path[el] = name[el]; el++; }
		p->exe_path[el] = '\0';
	}
}

/* Dynamically linked init: load its interpreter (ld.so) and enter that
 * instead — the loader relocates the image and jumps to the real entry,
 * which it reads from AT_ENTRY. Same contract execve(2) honours.
 * Nothing changes if it cannot be loaded. */
static void init_load_interp(process_t *p, const elf_info_t *ei, uint64_t *jump_entry,
                             uint64_t *at_base, uint64_t *interp_lo, uint64_t *interp_hi)
{
	vfs_stat_t ist = {0};
	if (vfs_stat(ei->interp, &ist) == 0 && ist.size > 0) {
		uint8_t *ibuf = kmalloc(ist.size);
		if (ibuf) {
			int ifd = vfs_open(ei->interp, O_RDONLY, 0);
			if (ifd >= 0) {
				vfs_read(ifd, ibuf, ist.size);
				vfs_close(ifd);
				elf_info_t ii = {0};
				if (elf_load(p->vm, ibuf, (size_t)ist.size, p->vm->mmap_next, &ii) == 0) {
					*jump_entry  = ii.entry;
					*at_base     = p->vm->mmap_next;
					*interp_lo   = ii.load_bias;
					*interp_hi   = ii.load_end;
					p->vm->mmap_next = (ii.load_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
				}
			}
			kfree(ibuf);
		}
	}
}

/* The user stack, and a page above it. -1: out of memory. */
static int init_map_stack(process_t *p)
{
	for (int i = 0; i < ARCH_USER_STACK_PAGES; i++) {
		void *spage_phys = buddy_alloc(0);
		if (!spage_phys) return -1;
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
	return 0;
}

/* argv = { name }, no envp, a short auxv: written straight into the new
 * address space while it is switched in; ctx enters at jump_entry. */
static void init_build_stack(process_t *p, const process_t *current, const char *name,
                             const elf_info_t *ei, uint64_t jump_entry, uint64_t at_base,
                             arch_context_t *ctx)
{
	/* Switch to new VM to write to user stack */
	vm_space_t *old_vm = current->vm;
	arch_vm_switch((uint64_t)p->vm->root, p->vm->asid);

	uint64_t sp = ARCH_USER_STACK_TOP;
	sp &= ~15ULL;

	sp -= 16;
	uint64_t at_random = sp;
	csprng_bytes((void *)at_random, 16);

	size_t nlen_full = 0;
	while (name[nlen_full] && nlen_full < PATH_MAX - 1) nlen_full++;
	sp -= nlen_full + 1;
	memcpy((char *)sp, name, nlen_full);
	((char *)sp)[nlen_full] = '\0';
	uint64_t at_execfn = sp;

	sp &= ~15ULL;

	/* auxv */
	sp -= 16; ((uint64_t*)sp)[0] = 0; ((uint64_t*)sp)[1] = 0; /* AT_NULL */
	sp -= 16; ((uint64_t*)sp)[0] = 9; ((uint64_t*)sp)[1] = ei->entry; /* AT_ENTRY */
	sp -= 16; ((uint64_t*)sp)[0] = 7; ((uint64_t*)sp)[1] = at_base; /* AT_BASE */
	sp -= 16; ((uint64_t*)sp)[0] = 6; ((uint64_t*)sp)[1] = PAGE_SIZE; /* AT_PAGESZ */
	sp -= 16; ((uint64_t*)sp)[0] = 5; ((uint64_t*)sp)[1] = ei->phnum; /* AT_PHNUM */
	sp -= 16; ((uint64_t*)sp)[0] = 4; ((uint64_t*)sp)[1] = ei->phentsize; /* AT_PHENT */
	sp -= 16; ((uint64_t*)sp)[0] = 3; ((uint64_t*)sp)[1] = ei->phdr_va; /* AT_PHDR */
	sp -= 16; ((uint64_t*)sp)[0] = 25; ((uint64_t*)sp)[1] = at_random; /* AT_RANDOM */

	sp -= 8; *(uint64_t*)sp = 0; /* envp NULL */
	sp -= 8; *(uint64_t*)sp = 0; /* argv NULL */
	sp -= 8; *(uint64_t*)sp = at_execfn; /* argv[0] */
	sp -= 8; *(uint64_t*)sp = 1; /* argc */

	arch_context_init_user(ctx, jump_entry, sp);

	if (old_vm) arch_vm_switch((uint64_t)old_vm->root, old_vm->asid);
	else arch_vm_switch((uint64_t)kernel_vm_space->root, 0);
}

pid_t proc_exec(const char *name, const void *elf_data, size_t elf_len)
{
	void *pcb_phys = buddy_alloc(PCB_ORDER);
	if (!pcb_phys) return -1;
	process_t *p = (process_t *)PHYS_TO_VIRT(pcb_phys);
	memset(p, 0, sizeof(*p));
	p->magic = PROC_MAGIC;
	p->start_tick = kernel_ticks;
	p->sighand = sighand_alloc(NULL);
	if (!p->sighand) { buddy_free(VIRT_TO_PHYS(p), PCB_ORDER); return -1; }

	process_t *current = task_current();
	p->ppid  = current->pid;
	p->state = PROC_READY;
	init_creds(p, current);

	init_names(p, name);

	p->vm = vm_create();
	if (!p->vm) goto fail;

	elf_info_t ei = {0};
	if (elf_load(p->vm, elf_data, elf_len, 0, &ei) != 0) {
		kprintf(KERN_WARN "init: elf_load failed\n");
		goto fail;
	}

	p->vm->brk      = ei.load_end;
	p->vm->mmap_next    = ARCH_USER_MMAP_BASE;
	p->vm->huge1g_next  = HUGE1G_VA_BASE;
	p->reaped       = 0;

	uint64_t jump_entry = ei.entry;
	uint64_t at_base    = 0;
	uint64_t interp_lo = 0, interp_hi = 0;
	if (ei.interp[0]) init_load_interp(p, &ei, &jump_entry, &at_base, &interp_lo, &interp_hi);

	if (init_map_stack(p)) goto fail;

	p->kstack = kstack_alloc();
	if (!p->kstack) goto fail;

	uint8_t *ksp = p->kstack + kstack_size_val;
	arch_context_t *ctx = (arch_context_t *)(ksp - sizeof(arch_context_t));

	init_build_stack(p, current, name, &ei, jump_entry, at_base, ctx);

	p->ctx = ctx;
	/* Record the initial VMAs (stack, image, interpreter) like execve does. The
	 * COW fault handler consults the VMA for write permission; without them a
	 * fork()ed child (or this process) faulting on a shared page is killed
	 * instead of getting its private copy. */
	vma_add(p, ARCH_USER_STACK_TOP - (uint64_t)ARCH_USER_STACK_PAGES * PAGE_SIZE,
	        (uint64_t)ARCH_USER_STACK_PAGES * PAGE_SIZE, VMA_USER | VMA_READ | VMA_WRITE);
	vma_add(p, ei.load_bias, ei.load_end - ei.load_bias,
	        VMA_USER | VMA_READ | VMA_WRITE | VMA_EXEC);
	if (interp_hi > interp_lo)
		vma_add(p, interp_lo, interp_hi - interp_lo,
		        VMA_USER | VMA_READ | VMA_WRITE | VMA_EXEC);

	p->fds = fd_table_create();
	/* pid 1 is handed fd 0/1/2 = /dev/console by the kernel — a
	 * freestanding C runtime (glibc) assumes the standard descriptors already
	 * exist and aborts otherwise. */
	if (p->fds) {
		mode_t um = fs_umask_of((struct process *)current);
		vfs_open_fds(p->fds, p->euid, p->egid, um, "/dev/console", O_RDONLY, 0);
		vfs_open_fds(p->fds, p->euid, p->egid, um, "/dev/console", O_WRONLY, 0);
		vfs_open_fds(p->fds, p->euid, p->egid, um, "/dev/console", O_WRONLY, 0);
	}
	/* the creator's root (chroot survives exec), cwd and umask; a cwd is
	 * always absolute (fs_struct starts at "/") */
	p->fs = fs_copy(current->fs);
	if (!p->fs) goto fail;

	{
		uint64_t f = spin_lock_irq(&sched_lock);
		int pid = proctab[1] ? alloc_pid() : 1;
		if (pid < 0) {
			spin_unlock_irq(&sched_lock, f);
			goto fail;
		}
		cgroup_can_fork(current->cgroup);   /* the root has no limit, so this holds */
		cgroup_fork_attach(p, current->cgroup);
		p->pid  = pid;
		p->tgid = pid;
		p->pgrp = pid;
		p->next = proctab[0]->next;
		proctab[0]->next = p;
		runqueue_add(p);
		proctab[pid] = p;
		pidns_attach(p, &init_pid_ns, pid);
		spin_unlock_irq(&sched_lock, f);
	}
	return p->pid;

fail:
	if (p->vm) vm_destroy(p->vm);
	if (p->fds) fd_table_destroy(p->fds);
	fs_put(p->fs);
	sighand_put(p->sighand);
	if (p->kstack) buddy_free(VIRT_TO_PHYS(p->kstack), kstack_order_val);
	buddy_free(VIRT_TO_PHYS(p), PCB_ORDER);
	return -1;
}

static void reap_dead_threads(void);

/* CLONE_FS: the same root, cwd and umask as the parent; else a copy of them. */
static fs_struct_t *task_fs_for_child(process_t *parent, int share)
{
	return share && parent->fs ? fs_get(parent->fs) : fs_copy(parent->fs);
}

/* What a new task carries over from its creator besides ids, memory, files and
 * its place in the file tree: the same for fork and for a new thread. */
static void copy_task_state(process_t *child, const process_t *parent)
{
	memcpy(child->exe_path, parent->exe_path, sizeof(child->exe_path));
	memcpy(child->saved_auxv, parent->saved_auxv, sizeof(child->saved_auxv));
	child->saved_auxv_len = parent->saved_auxv_len;

	/* a new task runs the same image: inherit the parent's argv so
	 * /proc/<pid>/cmdline (ps full command) reflects the real command line. */
	if (parent->cmdline && parent->cmdline_len) {
		child->cmdline = kmalloc(parent->cmdline_len);
		if (child->cmdline) {
			memcpy(child->cmdline, parent->cmdline, parent->cmdline_len);
			child->cmdline_len = parent->cmdline_len;
		}
	}
	child->oom_score_adj = parent->oom_score_adj;
	child->uname_linux   = parent->uname_linux;
	child->ctty          = parent->ctty;
	child->ctty_fops     = parent->ctty_fops;
}

/* The child gets its own (COW) address space, so deep-copy the parent space's
 * VMA list into the child space. Threads share a space and thus its VMAs;
 * only this fork-a-new-space path duplicates them. Read the parent list under
 * its fault_lock so a sibling thread's concurrent mmap/munmap can't tear it.
 * -1: out of memory. */
static int fork_copy_vmas(process_t *child, process_t *parent)
{
	uint64_t vlf = spin_lock_irq(&parent->vm->fault_lock);
	vma_t **dst = &child->vm->vmas;
	for (vma_t *v = parent->vm->vmas; v; v = v->next) {
		vma_t *nv = kmalloc(sizeof(*nv));
		if (!nv) { spin_unlock_irq(&parent->vm->fault_lock, vlf); return -1; }
		*nv = *v; nv->next = NULL;
		*dst = nv; dst = &nv->next;
	}
	spin_unlock_irq(&parent->vm->fault_lock, vlf);
	return 0;
}

/* A new task's first frame: the parent's syscall frame returning 0 (from
 * clone, for a thread), on the stack the caller gave if any. */
static void new_task_ctx(process_t *child, process_t *parent, uint64_t newsp)
{
	uint8_t *ksp = child->kstack + kstack_size_val;
	arch_context_t *cctx = (arch_context_t *)(ksp - sizeof(arch_context_t));
	memcpy(cctx, parent->in_syscall ? parent->uctx : parent->ctx, sizeof(arch_context_t));   /* a sleep moves ctx */
	CTX_SYSCALL_RET(cctx, 0);
	/* clone/vfork with an explicit child stack (e.g. glibc posix_spawn passes
	 * CLONE_VM|CLONE_VFORK + a stack): the child must run on it, not inherit
	 * the parent's SP, or the two corrupt each other's frames. */
	if (newsp) {
#if defined(__aarch64__)
		cctx->sp = newsp;
#else
		cctx->rsp = newsp;
#endif
	}
	child->ctx = cctx;
}

/* The child's namespaces: new ones for the CLONE_NEW* flags, else the
 * parent's. -1: out of memory (the caller releases what was taken). */
static int fork_namespaces(process_t *child, process_t *parent, uint64_t clone_flags)
{
	if (clone_flags & CLONE_NEWUTS) {
		child->uts_ns = uts_ns_clone(parent->uts_ns);
		if (!child->uts_ns) return -1;
	} else {
		child->uts_ns = uts_ns_get(parent->uts_ns);
	}
	if (clone_flags & CLONE_NEWIPC) {
		child->ipc_ns = ipc_ns_clone(parent->ipc_ns);
		if (!child->ipc_ns) return -1;
	} else {
		child->ipc_ns = ipc_ns_get(parent->ipc_ns);
	}
	/* a pending time ns (from the parent's unshare) applies to its children */
	child->time_ns = parent->time_ns_new
	               ? time_ns_get(parent->time_ns_new)
	               : time_ns_get(parent->time_ns);
	/* CLONE_NEWNS: the child's own copy of the mount table, as unshare makes */
	if (clone_flags & CLONE_NEWNS) {
		child->mnt_ns = mnt_ns_clone(parent->mnt_ns);
		if (!child->mnt_ns) return -1;
	} else {
		child->mnt_ns = mnt_ns_get(parent->mnt_ns);
	}
	if (clone_flags & CLONE_NEWUSER) {
		child->user_ns = user_ns_create_child(parent->user_ns ? parent->user_ns : &init_user_ns, parent->euid);
		if (!child->user_ns) return -1;
		child->cap_effective = child->cap_permitted = CAP_FULL;
	} else {
		child->user_ns = user_ns_get(parent->user_ns);
	}
	if (clone_flags & CLONE_NEWNET) {
		child->net_ns = net_ns_create();
		if (!child->net_ns) return -1;
	} else {
		child->net_ns = net_ns_get(parent->net_ns);
	}
	child->cgroup_ns = (clone_flags & CLONE_NEWCGROUP)
	                 ? cgroup_ns_create()
	                 : cgroup_ns_get(parent->cgroup_ns);
	return 0;
}

/* Make the child a task: charge it, give it a pid, put it on the lists and the
 * run queue, all under sched_lock. Returns its pid as the caller sees it, or
 * -EAGAIN/-ENOMEM with nothing published. */
static pid_t fork_publish(process_t *child, process_t *parent, struct asess *enter,
                          pid_namespace_t *target_pidns)
{
	pid_t ret;
	uint64_t f = spin_lock_irq(&sched_lock);
	/* Charged under sched_lock, with the publish below: proc_move_cgroup
	 * holds it too, so parent->cgroup cannot change between the two. */
	if (asess_fork(child, parent, enter) != 0) {
		spin_unlock_irq(&sched_lock, f);
		return -EAGAIN;
	}
	void *cg = enter ? asess_cgroup(enter) : parent->cgroup;
	if (cgroup_can_fork(cg) != 0) {
		asess_fork_cancel(child);
		spin_unlock_irq(&sched_lock, f);
		return -EAGAIN;
	}
	int pid = alloc_pid();
	if (pid < 0) {
		asess_fork_cancel(child);
		cgroup_cancel_fork(cg);
		spin_unlock_irq(&sched_lock, f);
		return -ENOMEM;
	}
	cgroup_fork_attach(child, cg);
	child->pid  = pid;
	child->tgid = pid;
	child->vfork_waiter  = parent->vfork_pending ? parent->pid : 0;
	child->vfork_pending = 0;
	child->pgrp = enter ? pid : parent->pgrp;
	if (enter) child->sid = pid;
	seccomp_fork(child, parent);
	child->next = proctab[0]->next;
	proctab[0]->next = child;
	ptrace_follow_fork(parent, child);
	runqueue_add(child);
	proctab[pid] = child;
	pidns_attach(child, target_pidns, pid);
	/* setns can aim a child at a pid ns the parent cannot see */
	ret = task_pid_nr_ns(child, current_pid_ns());
	if (!ret) ret = pid;
	spin_unlock_irq(&sched_lock, f);
	return ret;
}

/* Release what a half-made forked child holds (not the PCB, not its pid ns). */
static void fork_undo(process_t *child)
{
	fs_put(child->fs);
	cpu_affinity_free(child);
	proc_cmdline_free(child);
	if (child->vm)     vm_destroy(child->vm);
	if (child->fds)    fd_table_destroy(child->fds);
	sighand_put(child->sighand);
	if (child->kstack) buddy_free(VIRT_TO_PHYS(child->kstack), kstack_order_val);
	if (child->uts_ns) uts_ns_put(child->uts_ns);
	if (child->ipc_ns) ipc_ns_put(child->ipc_ns);
	if (child->time_ns) time_ns_put(child->time_ns);
	if (child->mnt_ns) mnt_ns_put(child->mnt_ns);
	if (child->user_ns) user_ns_put(child->user_ns);
	if (child->net_ns) net_ns_put(child->net_ns);
	if (child->cgroup_ns) cgroup_ns_put(child->cgroup_ns);
}

static pid_t do_fork(uint64_t clone_flags, uint64_t newsp, struct asess *enter)
{
	reap_dead_threads();
	process_t *parent = task_current();
	long err = -12;   /* out of memory, not the EPERM a bare -1 would read as */
	/* Checked here, not in the syscall gate: clone3 passes its flags in user
	 * memory, which another thread can change after a gate has read them. */
	if (parent->asess && (clone_flags & ASESS_CLONE_DENIED)) return -EPERM;
	/* the low byte of clone flags is the exit signal, not CLONE_NEWTIME */
	if (!enter && !may_create_ns(parent, clone_flags & ~0xffUL)) return -EPERM;
	/* a shared root and cwd would reach across the new mount namespace (Linux EINVAL) */
	if ((clone_flags & (CLONE_NEWNS | CLONE_FS)) == (CLONE_NEWNS | CLONE_FS)) return -EINVAL;
	arch_save_tls(parent);

	/* RLIMIT_NPROC: a non-root caller may not exceed its soft cap on live
	 * processes owned by its real uid. Only enforced once the limit is set
	 * (rlim_set), so the default stays unbounded; root is exempt. */
	if (parent->euid != 0 && parent->rlim_set) {
		uint64_t cap = parent->rlim_cur[RLIMIT_NPROC];
		if (cap != (uint64_t)-1 && (uint64_t)proc_count_uid(parent->uid) >= cap)
			return -EAGAIN;
	}

	void *pcb_phys = buddy_alloc(PCB_ORDER);
	if (!pcb_phys) return -ENOMEM;
	process_t *child = (process_t *)PHYS_TO_VIRT(pcb_phys);
	memset(child, 0, sizeof(*child));
	child->magic = PROC_MAGIC;
	child->start_tick = kernel_ticks;

	/* pick the child's pid namespace: a fresh one for clone(CLONE_NEWPID), the
	 * parent's pending ns (unshare CLONE_NEWPID), else the parent's own. */
	pid_namespace_t *target_pidns;
	int owns_pidns = 0;
	if (clone_flags & CLONE_NEWPID) {
		target_pidns = pid_ns_create_child(parent->pid_ns ? parent->pid_ns : &init_pid_ns);
		if (!target_pidns) { buddy_free(VIRT_TO_PHYS(child), PCB_ORDER); return -ENOMEM; }
		owns_pidns = 1;
	} else {
		target_pidns = parent->pid_ns_new ? parent->pid_ns_new
		             : (parent->pid_ns ? parent->pid_ns : &init_pid_ns);
	}

	child->ppid      = parent->tgid;   /* a child belongs to the group, not the thread */
	child->pgrp      = parent->pgrp;
	child->sid       = parent->sid;
	child->state     = PROC_READY;
	copy_creds(child, parent);   /* pdeathsig is NOT inherited */
	/* the child's slack, current and default, is the parent's current one */
	child->timer_slack = child->timer_slack_default = parent->timer_slack ? parent->timer_slack
	                                                : parent->timer_slack_default;
	child->mlock_future = parent->mlock_future;
	child->cpu_affinity = cpu_affinity_dup(parent->cpu_affinity);
	child->reaped    = 0;

	int nlen = 0;
	while (parent->name[nlen] && nlen < PROC_NAME_MAX - 3) nlen++;
	memcpy(child->name, parent->name, nlen);
	child->name[nlen++] = '_';
	child->name[nlen++] = 'c';
	child->name[nlen] = '\0';

	/* expensive ops: vm_copy (TLB flush), fd dup, VMA list — all outside sched_lock */
	child->vm = vm_copy(parent->vm);
	if (!child->vm) goto fail_early;
	/* Its pages stay charged where they are; its page tables are new. */
	child->vm->pt_charged = parent->vm->pt_charged;
	if (cgroup_charge_mem(child->vm, enter ? asess_cgroup(enter) : parent->cgroup,
	                      child->vm->pt_charged) != 0) {
		child->vm->pt_charged = 0;
		err = -12;
		goto fail_early;
	}

	child->fds = fd_table_copy(parent->fds);
	if (!child->fds) goto fail_early;
	child->fs = task_fs_for_child(parent, clone_flags & CLONE_FS);
	if (!child->fs) goto fail_early;
	copy_task_state(child, parent);

	if (fork_copy_vmas(child, parent)) goto fail_early;

	child->sighand = sighand_alloc(parent->sighand);   /* fork → private copy */
	if (!child->sighand) goto fail_early;
	child->sig_mask = parent->sig_mask;
	child->altstack = parent->altstack;   /* fork keeps it; a thread starts without one */

	child->kstack = kstack_alloc();
	if (!child->kstack) goto fail_early;

	new_task_ctx(child, parent, newsp);
	child->tls_base        = parent->tls_base;
	child->user_gs_base    = parent->user_gs_base;
	if (fork_namespaces(child, parent, clone_flags)) goto fail_early;
	child->ticks_remaining = sched_quantum_val;

	if (enter) {
		err = asess_prepare_child(child, enter);
		if (err) goto fail_early;
		err = -12;
	}

	pid_t ret = fork_publish(child, parent, enter, target_pidns);
	if (ret < 0) { err = ret; goto fail_early; }
	if (owns_pidns) pid_ns_put(target_pidns);   /* child holds its own ref now */
	return ret;

fail_early:
	fork_undo(child);
	if (owns_pidns) pid_ns_put(target_pidns);
	buddy_free(VIRT_TO_PHYS(child), PCB_ORDER);
	return err;
}

pid_t proc_fork(uint64_t clone_flags, uint64_t newsp)
{
	return do_fork(clone_flags, newsp, NULL);
}

pid_t proc_fork_asess(struct asess *s)
{
	return do_fork(ASESS_CLONE_NS, 0, s);
}

/* A thread has no parent to collect it, and an exited one keeps its pid and PCB
 * until someone does. Every new task clears them, so they cannot pile up. */
static void reap_dead_threads(void)
{
	while (dead_threads) {
		process_t *z = NULL;
		uint64_t f = spin_lock_irq(&sched_lock);
		for (process_t *c = proctab[0]->next; c && c != proctab[0]; c = c->next)
			if (c->state == PROC_ZOMBIE && !c->reaped && c->pid != c->tgid && !c->tracer) {
				z = c;
				break;
			}
		if (z) { z->reaped = 1; dead_threads--; }
		else dead_threads = 0;
		spin_unlock_irq(&sched_lock, f);
		if (!z) return;
		proc_reap(z);
	}
}

/* clone(CLONE_VM) without CLONE_THREAD: a process of its own that shares the
 * memory, the way a thread does, but has its own id group and its parent waits
 * for it. Set for the one clone in progress (the caller's syscall). */
pid_t proc_clone_thread_ex(uint64_t newsp, uint64_t tls, uint32_t *ctid, uint32_t *ptid, uint32_t *set_tid,
                           int as_process, int share_fs);

/* Who a new task is and what it may do, the same for fork and for a new thread:
 * one place, so the two paths cannot drift apart again (a thread once started
 * with unset, i.e. infinite, limits). */
void copy_creds(process_t *child, const process_t *parent)
{
	child->uid  = parent->uid;  child->euid = parent->euid; child->suid = parent->suid;
	child->gid  = parent->gid;  child->egid = parent->egid; child->sgid = parent->sgid;
	child->ngroups = parent->ngroups;
	memcpy(child->groups, parent->groups, sizeof child->groups);
	child->cap_permitted   = parent->cap_permitted;
	child->cap_effective   = parent->cap_effective;
	child->cap_inheritable = parent->cap_inheritable;
	child->no_new_privs = parent->no_new_privs;   /* sticky */
	child->mdwe         = parent->mdwe;           /* irreversible */
	child->dumpable     = parent->dumpable;
	child->rlim_set     = parent->rlim_set;
	memcpy(child->rlim_cur, parent->rlim_cur, sizeof child->rlim_cur);
	memcpy(child->rlim_max, parent->rlim_max, sizeof child->rlim_max);
}

pid_t proc_clone_thread(uint64_t newsp, uint64_t tls, uint32_t *ctid, uint32_t *ptid, uint32_t *set_tid)
{
	return proc_clone_thread_ex(newsp, tls, ctid, ptid, set_tid, 0, 1);
}

/* Make a new thread (or clone(CLONE_VM) process) a task, under sched_lock:
 * charge it, give it a tid, write the tid where the caller asked, run it.
 * Returns the tid as the caller sees it, or -EAGAIN/-ENOMEM with nothing
 * published. */
static pid_t thread_publish(process_t *child, process_t *parent, int as_process,
                            uint32_t *ptid, uint32_t *set_tid)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	if (asess_fork(child, parent, NULL) != 0) {
		spin_unlock_irq(&sched_lock, f);
		return -EAGAIN;
	}
	if (cgroup_can_fork(parent->cgroup) != 0) {
		asess_fork_cancel(child);
		spin_unlock_irq(&sched_lock, f);
		return -EAGAIN;
	}
	int pid = alloc_pid();
	if (pid < 0) {
		asess_fork_cancel(child);
		cgroup_cancel_fork(parent->cgroup);
		spin_unlock_irq(&sched_lock, f);
		return -ENOMEM;
	}
	cgroup_fork_attach(child, parent->cgroup);
	child->pid  = pid;
	child->tgid = as_process ? pid : parent->tgid;
	if (as_process) child->ppid = parent->tgid;
	child->vfork_waiter  = 0;
	child->vfork_pending = 0;
	if (parent->group_exiting) {   /* created behind task_group_exit's walk */
		child->group_exiting = 1;
		child->exit_code     = parent->exit_code;
		child->killed_by_sig = parent->killed_by_sig;
		child->sig_pending  |= 1UL << (SIGKILL - 1);
	}
	seccomp_fork(child, parent);
	child->next      = proctab[0]->next;
	proctab[0]->next = child;
	proctab[pid] = child;
	pidns_attach(child, parent->pid_ns ? parent->pid_ns : &init_pid_ns, pid);
	pid_t vtid = task_vpid(child);
	/* Write the real child TID before the child is runnable, for the flags
	 * that demand it at creation: CLONE_PARENT_SETTID (*ptid) and
	 * CLONE_CHILD_SETTID (*set_tid). musl keys __tl_lock on
	 * pthread_self()->tid (via PARENT_SETTID), so a 0 tid would silently
	 * disable thread-list locking under SMP.
	 * The CLONE_CHILD_CLEARTID address (ctid) is NOT written here — it is
	 * cleared to 0 (and futex-woken) only on exit. musl points it at the
	 * global __thread_list_lock; writing the tid into it at creation would
	 * set the lock to the new thread's own tid, making that thread's
	 * __tl_lock take the recursive (no-op) path and corrupt the list. */
	if (ptid)    put_user((uint32_t)vtid, ptid);       /* a fault here is the caller's: Linux ignores it too */
	if (set_tid) put_user((uint32_t)vtid, set_tid);
	/* Publish the tid (and all child setup) to memory before the child can
	 * be observed runnable on another CPU. On weakly-ordered aarch64 a stale
	 * tid=0 read makes musl's __tl_lock a no-op and corrupts the thread list. */
	smp_mb();
	ptrace_follow_clone(parent, child);
	runqueue_add(child);

	spin_unlock_irq(&sched_lock, f);
	return vtid;
}

/* Release what a half-made thread holds (not the PCB). */
static void thread_undo(process_t *child)
{
	fs_put(child->fs);
	cpu_affinity_free(child);
	proc_cmdline_free(child);
	if (child->vm) vm_destroy(child->vm);
	if (child->fds) fd_table_put(child->fds);
	sighand_put(child->sighand);
	if (child->kstack) buddy_free(VIRT_TO_PHYS(child->kstack), kstack_order_val);
	if (child->uts_ns) uts_ns_put(child->uts_ns);
	if (child->ipc_ns) ipc_ns_put(child->ipc_ns);
	if (child->time_ns) time_ns_put(child->time_ns);
	if (child->mnt_ns) mnt_ns_put(child->mnt_ns);
	if (child->user_ns) user_ns_put(child->user_ns);
	if (child->net_ns) net_ns_put(child->net_ns);
	if (child->cgroup_ns) cgroup_ns_put(child->cgroup_ns);
}

pid_t proc_clone_thread_ex(uint64_t newsp, uint64_t tls, uint32_t *ctid, uint32_t *ptid, uint32_t *set_tid,
                           int as_process, int share_fs)
{
	reap_dead_threads();
	process_t *parent = task_current();
	long err = -12;   /* out of memory, not the EPERM a bare -1 would read as */
	void *pcb_phys = buddy_alloc(PCB_ORDER);
	if (!pcb_phys) return -ENOMEM;
	process_t *child  = (process_t *)PHYS_TO_VIRT(pcb_phys);
	memset(child, 0, sizeof(*child));
	child->magic = PROC_MAGIC;
	child->start_tick = kernel_ticks;

	child->ppid         = parent->ppid;   /* a thread has the group's parent */
	child->pgrp         = parent->pgrp;
	child->sid          = parent->sid;
	child->state        = PROC_READY;
	copy_creds(child, parent);   /* limits too: they are the group's */
	child->cpu_affinity = cpu_affinity_dup(parent->cpu_affinity);

	int nlen = 0;
	while (parent->name[nlen] && nlen < PROC_NAME_MAX - 3) nlen++;
	memcpy(child->name, parent->name, nlen);
	child->name[nlen] = '\0';

	/* Threads share the VM space */
	child->vm = vm_get(parent->vm);
	if (!child->vm) goto fail;

	/* Threads share the fd table — bump the table's refcount */
	/* a clone(CLONE_VM) process without CLONE_FILES gets its own table, as after fork */
	child->fds = (as_process && !(as_process & 0x400)) ? fd_table_copy(parent->fds)
	                                                    : fd_table_get(parent->fds);
	if (!child->fds) goto fail;
	child->fs = task_fs_for_child(parent, share_fs);
	if (!child->fs) goto fail;

	copy_task_state(child, parent);

	/* Threads share signal dispositions (CLONE_SIGHAND) but each has its own
	 * blocked mask, inherited from the creator. */
	child->sighand  = (as_process && !(as_process & 0x800)) ? sighand_alloc(parent->sighand)
	                                                        : sighand_get(parent->sighand);
	if (!child->sighand) goto fail;
	child->sig_mask = parent->sig_mask;

	child->kstack = kstack_alloc();
	if (!child->kstack) goto fail;

	new_task_ctx(child, parent, newsp);   /* starts at clone() returning 0 */

	child->tls_base = tls ? tls : parent->tls_base;
	child->user_gs_base = parent->user_gs_base;
	child->uts_ns   = uts_ns_get(parent->uts_ns);
	child->ipc_ns   = ipc_ns_get(parent->ipc_ns);
	child->time_ns  = time_ns_get(parent->time_ns);
	child->mnt_ns   = mnt_ns_get(parent->mnt_ns);
	child->user_ns  = user_ns_get(parent->user_ns);
	child->net_ns   = net_ns_get(parent->net_ns);
	child->cgroup_ns = cgroup_ns_get(parent->cgroup_ns);

	child->clear_child_tid = ctid;
	child->ticks_remaining = sched_quantum_val;

	pid_t vtid = thread_publish(child, parent, as_process, ptid, set_tid);
	if (vtid < 0) { err = vtid; goto fail; }
	return vtid;

fail:
	thread_undo(child);
	buddy_free(VIRT_TO_PHYS(child), PCB_ORDER);
	return err;
}

/* Threads of a group share one cgroup: sched_lock keeps a clone or an exit from
 * slipping past the count, and a group member with no cgroup has already left. */
int proc_move_cgroup(process_t *t, void *cg)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *head = proctab[0];
	void *from = NULL;
	vm_space_t *vm = NULL;
	long n = 0;
	for (process_t *c = head->next; c && c != head; c = c->next) {
		if (c->tgid != t->tgid || !c->cgroup) continue;
		from = c->cgroup;
		if (c->vm) vm = c->vm;
		n++;
	}
	int r = !n ? -3 : from == cg ? 0 : cgroup_migrate(from, cg, n, vm);
	if (r == 0)
		for (process_t *c = head->next; c && c != head; c = c->next)
			if (c->tgid == t->tgid && c->cgroup) c->cgroup = cg;
	spin_unlock_irq(&sched_lock, f);
	return r;
}

process_t *proc_find(pid_t pid)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *p = (pid < 0 || pid >= proc_cnt) ? NULL : proctab[pid];
	spin_unlock_irq(&sched_lock, f);
	return p;
}

/*
 * get_task_struct/put_task_struct equivalent. proc_find returns a raw pointer
 * that proc_reap can free the moment sched_lock is dropped; a caller that
 * dereferences the result after that (procfs fills, ptrace, …) races a
 * concurrent exit. proc_get pins the PCB under sched_lock so reap defers the
 * free, and proc_put releases the pin (running the deferred free when it is the
 * last one). refcount counts only outstanding pins; the PCB is freed when the
 * count reaches 0 after proc_reap has marked it reaped.
 */
process_t *proc_get(pid_t pid)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *p = (pid < 0 || pid >= proc_cnt) ? NULL : proctab[pid];
	if (p) p->refcount++;
	spin_unlock_irq(&sched_lock, f);
	return p;
}

void proc_hold(process_t *p)
{
	if (!p) return;
	uint64_t f = spin_lock_irq(&sched_lock);
	p->refcount++;
	spin_unlock_irq(&sched_lock, f);
}

void proc_put(process_t *p)
{
	if (!p) return;
	uint64_t f = spin_lock_irq(&sched_lock);
	int free_now = (--p->refcount == 0 && p->pcb_dead);
	spin_unlock_irq(&sched_lock, f);
	if (free_now) {
		cpu_affinity_free(p);
		proc_cmdline_free(p);
		p->magic = 0;
		buddy_free(VIRT_TO_PHYS(p), PCB_ORDER);
	}
}

/*
 * Pin a target process's address space for cross-process access
 * (/proc/<pid>/mem). task_exit steals p->vm under sched_lock and only then
 * vm_destroy()s it, so grabbing a vm_get() ref under the same lock races it
 * safely: either we ref it before the steal (the exit's vm_destroy defers to
 * our ref) or we already see NULL and return it. The space is walked via the
 * direct map, never activated, so it is safe to use from another process's
 * context. Release with vm_destroy().
 */
vm_space_t *proc_get_vm(pid_t pid)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *p = (pid < 0 || pid >= proc_cnt) ? NULL : proctab[pid];
	vm_space_t *vm = (p && p->vm) ? vm_get(p->vm) : NULL;
	spin_unlock_irq(&sched_lock, f);
	return vm;
}

/* As proc_get_vm, but also samples the target's euid under the same lock — for
 * the access check in process_vm_readv/writev. */
vm_space_t *proc_get_vm_cred(pid_t pid, uid_t *out_euid)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *p = (pid < 0 || pid >= proc_cnt) ? NULL : proctab[pid];
	vm_space_t *vm = (p && p->vm) ? vm_get(p->vm) : NULL;
	if (p && out_euid) *out_euid = p->euid;
	spin_unlock_irq(&sched_lock, f);
	return vm;
}

/* Linux cap_emulate_setxuid: when a process drops the last uid 0 from its
 * {real,effective,saved} set, its permitted/effective caps are cleared; when
 * only euid leaves 0, the effective set is cleared. Call after a setuid-family
 * change with the pre-change root status and euid. Keeps a su/setuid drop from
 * silently retaining CAP_SYS_PTRACE et al. */
void cred_setxuid_fixup(process_t *p, int was_priv, uid_t old_euid)
{
	int has_root = (p->uid == 0 || p->euid == 0 || p->suid == 0);
	if (was_priv && !has_root) {
		p->cap_permitted   = 0;
		p->cap_effective   = 0;
		p->cap_inheritable = 0;
	} else if (old_euid == 0 && p->euid != 0) {
		p->cap_effective = 0;
	}
	/* Linux cap_emulate_setxuid: a temporary drop (seteuid(user)) comes back
	 * with seteuid(0), and the effective set with it */
	if (old_euid != 0 && p->euid == 0)
		p->cap_effective = p->cap_permitted;
}

/* ptrace_may_access gate for cross-process /proc readers (maps, mem). Pins the
 * target so the credential read is safe, then applies the ptrace permission
 * rule. Reading one's own /proc entry always passes. */
int proc_may_access(pid_t pid)
{
	process_t *t = proc_get(pid);
	if (!t) return 0;
	int ok = ptrace_may_access(t);
	proc_put(t);
	return ok;
}

/*
 * Pin a target process's fd table for /proc/<pid>/fd readers. Same race
 * discipline as proc_get_vm: task_exit steals p->fds under sched_lock before
 * fd_table_put()ing it, so an fd_table_get() under the lock either refs it
 * first or sees NULL. Reading proctab[pid] under the lock also never touches a
 * reaped PCB (proc_reap NULLs the slot under the lock before freeing). Release
 * with fd_table_put().
 */
fd_table_t *proc_get_fds(pid_t pid)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *p = (pid < 0 || pid >= proc_cnt) ? NULL : proctab[pid];
	fd_table_t *fds = (p && p->fds) ? fd_table_get(p->fds) : NULL;
	spin_unlock_irq(&sched_lock, f);
	return fds;
}

/*
 * Pin a target's namespace for an ns-link open (/proc/<pid>/ns/<kind>). Same
 * race discipline as proc_get_vm: task_exit steals the ns pointer under
 * sched_lock before xxx_ns_put()ing it, so reading it + xxx_ns_get() here under
 * the lock either refs it first or sees NULL. Release with the matching put.
 */
void *proc_get_ns(pid_t pid, int kind)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *p = (pid < 0 || pid >= proc_cnt) ? NULL : proctab[pid];
	void *ns = NULL;
	if (p) switch (kind) {
	case PROC_NS_UTS:    ns = uts_ns_get(p->uts_ns);       break;
	case PROC_NS_IPC:    ns = ipc_ns_get(p->ipc_ns);       break;
	case PROC_NS_TIME:   ns = time_ns_get(p->time_ns);     break;
	case PROC_NS_MNT:    ns = mnt_ns_get(p->mnt_ns);       break;
	case PROC_NS_PID:    ns = pid_ns_get(p->pid_ns);       break;
	case PROC_NS_USER:   ns = user_ns_get(p->user_ns);     break;
	case PROC_NS_NET:    ns = net_ns_get(p->net_ns);       break;
	case PROC_NS_CGROUP: ns = cgroup_ns_get(p->cgroup_ns); break;
	}
	spin_unlock_irq(&sched_lock, f);
	return ns;
}

/* Count live threads sharing a thread group (for /proc/<pid>/status Threads). */
process_t *proc_other_threads_locked(process_t *p, int (*fn)(process_t *t, void *arg), void *arg)
{
	process_t *sentinel = proctab[0];
	for (process_t *t = sentinel->next; t && t != sentinel; t = t->next) {
		if (t == p || t->tgid != p->tgid || t->state == PROC_DEAD || t->state == PROC_ZOMBIE)
			continue;
		if (fn(t, arg)) return t;
	}
	return NULL;
}

int proc_thread_count(pid_t tgid)
{
	int n = 0;
	uint64_t f = spin_lock_irq(&sched_lock);
	for (int i = 0; i < proc_cnt; i++) {
		process_t *p = proctab[i];
		if (p && p->tgid == tgid && p->state != PROC_DEAD && p->state != PROC_ZOMBIE) n++;
	}
	spin_unlock_irq(&sched_lock, f);
	return n ? n : 1;
}

/* OOM-killer victim selection (policy in kernel/mm/oom.c). Pick the live user
 * process with the largest charged footprint, never init (pid 1) or a kernel
 * thread (no ->vm), nor one already condemned. Returns it pinned (proc_put to
 * release) so the policy can read/kill it after sched_lock drops. */
/* An OOM victim still dying: its memory is on the way back, killing more before it
 * lands only takes bystanders (sshd, the shell) with it. 1 if one exists, 2 if the
 * caller itself is it. */
int oom_victim_pending(void)
{
	int r = 0;
	process_t *me = task_current();
	uint64_t f = spin_lock_irq(&sched_lock);
	for (int i = 2; i < proc_cnt && r != 2; i++) {
		process_t *p = proctab[i];
		if (!p || p->magic != PROC_MAGIC || !p->oom_victim) continue;
		if (p->state == PROC_DEAD || p->state == PROC_ZOMBIE || !p->vm) continue;
		r = (me && p->vm == me->vm) ? 2 : 1;
	}
	spin_unlock_irq(&sched_lock, f);
	return r;
}

process_t *oom_select_victim(long *charged)
{
	uint64_t bit = 1UL << (SIGKILL - 1);
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *best = NULL;
	long best_mem = -1;
	for (int i = 2; i < proc_cnt; i++) {
		process_t *p = proctab[i];
		if (!p || !p->vm || p->magic != PROC_MAGIC) continue;
		if (p->state == PROC_DEAD || p->state == PROC_ZOMBIE) continue;
		if (p->sig_pending & bit) continue;   /* already condemned, will free soon */
		if (p->oom_score_adj <= -1000) continue;   /* asked never to be chosen */
		/* the adjustment shifts the score by its thousandths of all memory, as on Linux */
		long mem = p->vm->mem_charged + (long)p->oom_score_adj * (long)(buddy_total_pages() * 4096UL / 1000);
		if (mem > best_mem) { best_mem = mem; best = p; }
	}
	if (best) { best->refcount++; *charged = best_mem; }
	spin_unlock_irq(&sched_lock, f);
	return best;
}

/* SIGKILL a chosen OOM victim, lock-free (set the bit, inline-wake if asleep)
 * exactly as cgroup_oom_kill — no cross-CPU IPI / task_signal that could
 * deadlock against a held mm lock on the faulting path. */
void oom_kill_victim(process_t *v)
{
	if (!v) return;
	uint64_t f = spin_lock_irq(&sched_lock);
	v->sig_pending |= (1UL << (SIGKILL - 1));
	v->oom_victim = 1;
	if (v->state == PROC_SLEEPING) {
		v->state = PROC_READY;
		if (!v->on_cpu) runqueue_add(v);
		arch_get_cpu_ctx()->need_resched = 1;
	}
	spin_unlock_irq(&sched_lock, f);
}

/* Is p's root/cwd/umask shared with a live task outside its thread group
 * (CLONE_FS without CLONE_THREAD)? Linux check_unsafe_exec: a setuid exec must
 * then not raise ids, or that task could move the new image's root and cwd. */
int fs_shared_outside_group(process_t *p)
{
	fs_struct_t *fs = p->fs;
	if (!fs || fs->users <= 1) return 0;
	int found = 0;
	uint64_t f = spin_lock_irq(&sched_lock);
	for (int i = 1; i < proc_cnt && !found; i++) {
		process_t *t = proctab[i];
		if (!t || t->state == PROC_DEAD || t->state == PROC_ZOMBIE) continue;
		found = t->fs == fs && t->tgid != p->tgid;
	}
	spin_unlock_irq(&sched_lock, f);
	return found;
}

/* 1 if a live task's root lies at or under the dentry top */
int proc_root_under(const void *top)
{
	int found = 0;
	uint64_t f = spin_lock_irq(&sched_lock);
	for (int i = 1; i < proc_cnt && !found; i++) {
		process_t *p = proctab[i];
		if (!p || p->state == PROC_DEAD || p->state == PROC_ZOMBIE) continue;
		for (vfs_dentry_t *d = fs_root_of((struct process *)p); d && !found; d = d->parent)
			found = d == top;
	}
	spin_unlock_irq(&sched_lock, f);
	return found;
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
		poll_arm();
		poll_block((uint64_t)-1);
	}
}

/*
 * Called from assembly after the stack switch (mov sp, x0) in irq_handler and
 * .yield.  At that point we are on the NEW process's kstack, so it is safe to
 * release resources belonging to the process we just switched away from.
 */


/* A vfork child that execs or exits lets its parent go on. */
void vfork_release(process_t *c)
{
	pid_t w = c->vfork_waiter;
	if (!w) return;
	c->vfork_waiter = 0;
	process_t *pp = proc_get(w);
	if (pp) {
		pp->vfork_pending = 0;
		task_wake(pp);
		proc_put(pp);
	}
}

/* What the thread leaves in user memory, done while p->vm is still live and
 * outside sched_lock (futex before sched): clear_child_tid, robust futexes. */
static void exit_user_state(process_t *p)
{
	uint32_t *cctid = p->clear_child_tid;

	/* Write tid=0 and wake futex waiter while p->vm is still set.
	 * Must happen before stealing p->vm: a preemption between p->vm=NULL
	 * and this write would cause vm_activate to load the kernel PT, making
	 * the user-address write fault at EL1. futex_wake_addr must also be
	 * called outside sched_lock to preserve lock order (futex→sched). */
	if (cctid) {
		/* CLONE_CHILD_CLEARTID / set_tid_address: write 0 to the address and
		 * futex-wake it on exit. A threading library may use this address as a
		 * lock it holds across SYS_exit (released by this write); the thread's
		 * prior user writes must be globally visible before the release, and
		 * the 0 before the wake — release ordering the bare store lacks. */
		arch_get_cpu_ctx()->wd_where = "exit:ctid";
		smp_mb();
		uint32_t zero = 0;
		copy_to_user(cctid, &zero, sizeof(zero));   /* fault → ignore, like Linux put_user */
		smp_mb();
		arch_get_cpu_ctx()->wd_where = "exit:futex";
		futex_wake_addr(cctid);
	}

	/* Robust-mutex handoff: while p->vm is still live (the list and futex words
	 * are in user memory) and outside sched_lock (futex→sched order), tag every
	 * futex this thread still owns OWNER_DIED and wake a survivor. */
	arch_get_cpu_ctx()->wd_where = "exit:robust";
	futex_exit_robust(p);
	p->robust_list = NULL;
	seccomp_put(p->seccomp_filter);
	p->seccomp_filter = NULL;
}

/* Controlling-terminal hangup: when a session leader exits, SIGHUP+SIGCONT
 * its session and detach that ctty from every member. */
static void exit_hangup_session(process_t *p)
{
	if (!(p->ctty && p->sid == (pid_t)p->pid)) return;
	void *dead_ctty = p->ctty;
	pid_t dead_sid  = p->sid;
	uint64_t hf = spin_lock_irq(&sched_lock);
	process_t *hs = proctab[0];
	for (process_t *t = hs->next; t && t != hs; t = t->next) {
		if (t->ctty == dead_ctty && t->sid == dead_sid && t != p) {
			t->sig_pending |= (1UL << (SIGHUP - 1)) | (1UL << (SIGCONT - 1));
			t->sig_pending &= ~((1UL << (SIGTSTP - 1)) | (1UL << (SIGTTIN - 1)) |
			                    (1UL << (SIGTTOU - 1)));
			sig_from_forget(t, (1UL << (SIGTSTP - 1)) | (1UL << (SIGTTIN - 1)) |
			                   (1UL << (SIGTTOU - 1)));
			if (t->job_stopped) { t->job_stopped = 0; t->job_continued = 1; }
			if (t->state == PROC_SLEEPING) {
				t->state = PROC_READY;
				if (!t->on_cpu) runqueue_add(t);
				arch_get_cpu_ctx()->need_resched = 1;
			}
			t->ctty = NULL; t->ctty_fops = NULL;
		}
	}
	spin_unlock_irq(&sched_lock, hf);
}

/* Let go of the memory, files, signal table and namespaces. The pointers are
 * taken under sched_lock (a pinned-PCB /proc reader looks under it too); the
 * puts happen outside it, while the task is still schedulable. */
static void exit_release(process_t *p)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	vm_space_t *dying_vm  = p->vm;   p->vm  = NULL;
	fd_table_t *dying_fds = p->fds;  p->fds = NULL;
	sighand_t  *dying_sh  = p->sighand; p->sighand = NULL;
	fs_struct_t *dying_fs = p->fs;      p->fs = NULL;   /* fs_of_task reads it under this lock */
	/* Steal the namespace pointers under the same lock as vm/fds: a pinned-PCB
	 * /proc reader (proc_get_ns) reads ns + xxx_ns_get under sched_lock, so it
	 * either refs before the steal or sees NULL — never a dangling ns. */
	uts_namespace_t  *dying_uts  = p->uts_ns;     p->uts_ns     = NULL;
	ipc_namespace_t  *dying_ipc  = p->ipc_ns;     p->ipc_ns     = NULL;
	time_namespace_t *dying_time = p->time_ns;    p->time_ns    = NULL;
	time_namespace_t *dying_tnew = p->time_ns_new; p->time_ns_new = NULL;
	mnt_namespace_t  *dying_mnt  = p->mnt_ns;     p->mnt_ns     = NULL;
	user_namespace_t *dying_user = p->user_ns;    p->user_ns    = NULL;
	net_namespace_t  *dying_net  = p->net_ns;     p->net_ns     = NULL;
	struct cgroup_namespace *dying_cgns = p->cgroup_ns; p->cgroup_ns = NULL;
	struct asess *dying_asess = p->asess;         p->asess      = NULL;
	spin_unlock_irq(&sched_lock, f);

	asess_task_exit(dying_asess);
	uts_ns_put(dying_uts);
	ipc_ns_put(dying_ipc);
	time_ns_put(dying_time);
	if (dying_tnew) time_ns_put(dying_tnew);
	mnt_ns_put(dying_mnt);
	user_ns_put(dying_user);
	net_ns_put(dying_net);
	cgroup_ns_put(dying_cgns);

	/* Free resources — process is still schedulable during this. */
	sighand_put(dying_sh);
	if (dying_fs != &init_fs) fs_put(dying_fs);
	arch_get_cpu_ctx()->wd_where = "exit:fdput";
	if (dying_fds) fd_table_put(dying_fds);
	if (dying_vm) {
		arch_get_cpu_ctx()->wd_where = "exit:vmdestroy";
		vm_activate();
		vm_destroy(dying_vm);
	}
}

/* Under sched_lock: wake the tracer, then the parent with SIGCHLD. */
static void exit_notify(process_t *p, int group_left)
{
	if (p->tracer > 0 && (int)p->tracer < proc_cnt && proctab[p->tracer]) {
		process_t *t = proctab[p->tracer];     /* the tracer hears of the exit first */
		if (t->state == PROC_SLEEPING) {
			t->state = PROC_READY;
			if (!t->on_cpu) runqueue_add(t);
		}
		t->sig_pending |= 1UL << (SIGCHLD - 1);
	}
	/* A thread is nobody's child: no wakeup, no SIGCHLD. The process ends with its
	 * last thread, which need not be the leader. */
	if (!group_left || p->tracer) {
		process_t *parent = (p->ppid < proc_cnt) ? proctab[p->ppid] : NULL;
		if (parent) {
			if (parent->state == PROC_SLEEPING) {
				parent->state = PROC_READY;
				if (!parent->on_cpu) runqueue_add(parent);
				arch_get_cpu_ctx()->need_resched = 1;
			}
			parent->sig_pending |= (1UL << (SIGCHLD - 1));
		}
		wake_group_waiters(p->ppid);
	}
}

/* A pid namespace's init dying takes the whole namespace down: SIGKILL
 * every remaining member (standard CLONE_NEWPID semantics). Under sched_lock. */
static void exit_kill_pidns(process_t *p, pid_namespace_t *exit_pidns)
{
	exit_pidns->dead = 1;
	for (process_t *c = proctab[0]->next; c && c != proctab[0]; c = c->next)
		if (c != p && c->pid_ns == exit_pidns) {
			c->sig_pending |= (1UL << (SIGKILL - 1));
			if (c->state == PROC_SLEEPING) {
				c->state = PROC_READY;
				if (!c->on_cpu) runqueue_add(c);
				arch_get_cpu_ctx()->need_resched = 1;
			}
		}
}

/* Under sched_lock: whatever p was tracing runs free. */
static void exit_free_tracees(process_t *p)
{
	for (process_t *c = proctab[0]->next; c && c != proctab[0]; c = c->next)
		if (c->tracer == (pid_t)p->pid) {       /* its tracer is gone: run free */
			c->tracer = 0;
			c->ptrace_opts = c->ptrace_options = 0;
			c->ptrace_stop = 0;
			c->ptrace_in_syscall = 0;
			if (c->state == PROC_SLEEPING) {
				c->state = PROC_READY;
				if (!c->on_cpu) runqueue_add(c);
			}
		}
}

void task_exit(void)
{
	process_t *p = task_current();
	KASSERT(p->magic == PROC_MAGIC);
	tty_task_gone(p->pid);
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
	p->exiting = 1;
	vfork_release(p);
	if (p->killed_by_sig && !p->group_exiting)
		task_group_exit(p, p->exit_code, 1);
	/* release the process's fcntl record locks: they are the thread group's,
	 * so only the last thread using its fd table lets them go */
	if (!p->fds || (int32_t)p->fds->refcount <= 1)
		vfs_posix_drop_pid((int)p->tgid);

	exit_user_state(p);

	/* If this is a pid namespace's init (nr 1), capture that before detaching
	 * so the reparent section can tear the namespace down; hold an extra ref so
	 * it survives the put below. */
	pid_namespace_t *exit_pidns = p->pid_ns;
	int exit_is_reaper = exit_pidns && exit_pidns != &init_pid_ns && task_vpid(p) == 1;
	if (exit_is_reaper) pid_ns_get(exit_pidns);
	pidns_detach(p);
	if (p->pid_ns_new) { pid_ns_put(p->pid_ns_new); p->pid_ns_new = NULL; }
	pid_ns_put(p->pid_ns);
	p->pid_ns = NULL;
	{
		uint64_t cf = spin_lock_irq(&sched_lock);
		cgroup_exit(p);
		spin_unlock_irq(&sched_lock, cf);
	}
	sigqueue_drop(p);

	exit_hangup_session(p);

	exit_release(p);
	/* The VMA list belongs to the address space, freed by vm_destroy above when
	 * the last thread drops its reference — not per-thread here. */

	/* Cleanup done — now mark dead and wake parent. */
	arch_get_cpu_ctx()->wd_where = "exit:reparent";
	vfs_walk_done(p);
	uint64_t f = spin_lock_irq(&sched_lock);
	if (p->pid != p->tgid && p->tgid > 0 && p->tgid < proc_cnt && proctab[p->tgid]) {
		/* a thread's CPU time is its process's, and `time` reads it from the leader */
		proctab[p->tgid]->utime_ticks += p->utime_ticks;
		proctab[p->tgid]->stime_ticks += p->stime_ticks;
		p->utime_ticks = p->stime_ticks = 0;
	}
	p->state = PROC_DEAD;
	if (p->itimer_value) { sched_itimer_account(p->itimer_value, 0); p->itimer_value = 0; }

	/* Reparent orphaned children to init (pid 1) */
	process_t *init_proc = (proc_cnt > 1 && proctab[1]) ? proctab[1] : NULL;
	process_t *sentinel2 = proctab[0];
	int walk = 0, walk_max = proc_cnt + 8;
	int group_left = 0;   /* children stay with the group until its last thread goes */
	for (process_t *c = sentinel2->next; c && c != sentinel2; c = c->next)
		if (c != p && c->tgid == p->tgid && c->state != PROC_DEAD && c->state != PROC_ZOMBIE) {
			group_left = 1;
			break;
		}
	exit_free_tracees(p);
	if (!group_left) {
		ptimer_drop(p->tgid);
	}
	for (process_t *c = sentinel2->next; c && c != sentinel2; c = c->next) {
		if (++walk > walk_max)
			kpanic("task_exit: process list cycle (corrupt ->next)");
		if (!group_left && c->ppid == (pid_t)p->tgid && c != p && c->tgid != p->tgid) {
			/* orphans reparent to the reaper (nr 1) of their own pid ns */
			pid_namespace_t *cns = c->pid_ns;
			process_t *reaper = (cns && cns != &init_pid_ns && cns->reaper && cns->reaper != c &&
			                     cns->reaper != p)
			                  ? cns->reaper : init_proc;
			c->ppid = reaper ? (pid_t)reaper->pid : 0;
			if (c->pdeathsig > 0 && c->pdeathsig <= NSIG) {
				c->sig_pending |= 1UL << (c->pdeathsig - 1);
				if (c->state == PROC_SLEEPING) {
					c->state = PROC_READY;
					if (!c->on_cpu) runqueue_add(c);
				}
			}
			if ((c->state == PROC_DEAD || c->state == PROC_ZOMBIE) && reaper)
				reaper->sig_pending |= (1UL << (SIGCHLD - 1));
		}
	}

	if (exit_is_reaper) exit_kill_pidns(p, exit_pidns);

	exit_notify(p, group_left);

	spin_unlock_irq(&sched_lock, f);

	if (exit_is_reaper) pid_ns_put(exit_pidns);   /* drop the teardown ref */

	arch_yield();
}

/* The child of ppid that namespace ns numbers nr. Searches the process list: a
 * zombie has already left the namespace's map. */
process_t *proc_find_child(pid_t ppid, pid_namespace_t *ns, int nr)
{
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *sentinel = proctab[0];
	process_t *found = NULL;
	for (process_t *c = sentinel->next; c && c != sentinel; c = c->next) {
		if (((c->ppid == ppid && (c->pid == c->tgid || c->tracer)) ||
		     c->tracer == (pid_t)task_current()->pid) &&
		    task_pid_nr_ns(c, ns) == nr) { found = c; break; }
	}
	spin_unlock_irq(&sched_lock, f);
	return found;
}

/* A thread is not a child: wait(-1) must neither return one nor block on one. */
#define WAITABLE(c) ((c)->pid == (c)->tgid || (c)->tracer)

/* A leader gone by pthread_exit: its process lives on until the last thread ends,
 * and wait sees none of it before that. sched_lock held. */
static int group_running(process_t *leader)
{
	if (leader->pid != leader->tgid) return 0;
	for (process_t *t = proctab[0]->next; t && t != proctab[0]; t = t->next)
		if (t != leader && t->tgid == leader->tgid &&
		    t->state != PROC_DEAD && t->state != PROC_ZOMBIE)
			return 1;
	return 0;
}
/* wait sees its children and the tasks it traces, which may be nobody's child of it */
#define WAIT_MINE(c, ppid, me) (((c)->ppid == (ppid) && WAITABLE(c)) || (c)->tracer == (pid_t)(me)->pid)

void proc_find_zombie(pid_t ppid, process_t **out_child, int *out_any_alive)
{
	*out_child = NULL;
	*out_any_alive = 0;
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *sentinel = proctab[0];
	for (process_t *c = sentinel->next; c && c != sentinel; c = c->next) {
		if (c->ppid == ppid && WAITABLE(c)) {
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

/* event: 1 exit, 2/5 ptrace stop, 3 job stop, 4 continued (wait4 status words) */
static void wait_snap(process_t *c, int event, wait_snap_t *s)
{
	if (!s) return;
	if (event == 1)
		s->status = c->killed_by_sig ? (c->exit_code & 0xFF)   /* the signal, 0x80 for a core */
		                             : (c->exit_code & 0xFF) << 8;
	else if (event == 2 || event == 5)
		s->status = ((c->ptrace_stop_sig ? c->ptrace_stop_sig : 5 /* SIGTRAP */) << 8) | 0x7f;
	else if (event == 3)
		s->status = (c->job_stop_sig << 8) | 0x7f;
	else
		s->status = 0xffff;
	int v = task_pid_nr_ns(c, current_pid_ns());
	s->nr  = v ? v : (int)c->pid;
	s->uid = c->uid;
	s->trapped = event == 2 || event == 5;
}

/* Like proc_find_zombie but, if no zombie is found and will_sleep is set,
 * atomically marks parent PROC_SLEEPING under sched_lock so that a
 * concurrent task_exit cannot sneak its wakeup between our check and sleep.
 * event: 0 none, 1 zombie (reaped), 3 stopped (WUNTRACED), 4 continued
 * (WCONTINUED), 5 ptrace stop. An exit counts only with WEXITED; with WNOWAIT
 * nothing is marked reaped or reported, so the next wait sees it again. */
void proc_find_zombie_or_sleep(pid_t ppid, process_t *parent, int will_sleep, int flags,
                               process_t **out_child, int *out_any_alive, int *out_event,
                               wait_snap_t *snap)
{
	*out_child = NULL;
	*out_any_alive = 0;
	*out_event = 0;
	int keep = (flags & WNOWAIT) != 0;
	uint64_t f = spin_lock_irq(&sched_lock);
	process_t *sentinel = proctab[0];
	int dead_pending = 0;
	for (process_t *c = sentinel->next; c && c != sentinel; c = c->next) {
		if (WAIT_MINE(c, ppid, parent)) {
			int ev = 0;
			int held = (c->tracer && c->tracer != (pid_t)parent->pid) || group_running(c);
			if (c->tracer == (pid_t)parent->pid && c->ptrace_stop && !c->ptrace_stop_reported) {
				if (!keep) c->ptrace_stop_reported = 1;
				ev = 5;
			} else if (c->state == PROC_ZOMBIE && !c->reaped && !held && (flags & WEXITED)) {
				if (!keep) c->reaped = 1;
				ev = 1;
			} else if ((flags & WUNTRACED) && c->job_stopped && !c->job_stop_reported) {
				if (!keep) c->job_stop_reported = 1;
				ev = 3;
			} else if ((flags & WCONTINUED) && c->job_continued) {
				if (!keep) c->job_continued = 0;
				ev = 4;
			}
			if (ev) {
				wait_snap(c, ev, snap);
				*out_child = c;
				*out_event = ev;
				break;
			}
			if (c->state == PROC_DEAD)
				dead_pending = 1;   /* exiting; becomes ZOMBIE shortly */
			/* a tracer hears of an exit before the parent may reap it */
			if (c->state != PROC_ZOMBIE || held)
				*out_any_alive = 1;
		}
	}
	/* Don't sleep while a child is mid-exit (PROC_DEAD): its task_exit wakeup
	 * has already fired, and it only turns ZOMBIE later in schedule(). Yield
	 * and retry instead, or the wakeup is lost forever. */
	if (!*out_child && *out_any_alive && will_sleep && !dead_pending) {
		parent->state = PROC_SLEEPING;
		parent->sleeping_on_queue = 1;  /* woken by task_exit, not the per-tick poll */
		parent->in_wait = 1;
	}
	spin_unlock_irq(&sched_lock, f);
}

/* Atomically check child state; if there is nothing to report and will_sleep
 * is set, marks parent PROC_SLEEPING under the same lock hold that checked the
 * child — no wakeup can slip through.
 * zombie=1, ptrace=2, job-stopped(WUNTRACED)=3, continued(WCONTINUED)=4,
 * sleep=0, -1: a zombie but no WEXITED (nothing will ever come: ECHILD). */
int proc_check_or_sleep(process_t *child, process_t *parent, int will_sleep, int flags,
                        wait_snap_t *snap)
{
	int keep = (flags & WNOWAIT) != 0;
	uint64_t f = spin_lock_irq(&sched_lock);
	int zombie = (child->state == PROC_ZOMBIE) && !group_running(child) &&
	             !(child->tracer && child->tracer != (pid_t)parent->pid);
	int ptrace = !zombie && child->ptrace_stop && !child->ptrace_stop_reported &&
	             child->tracer == (pid_t)parent->pid;
	int jstop  = !zombie && !ptrace && (flags & WUNTRACED)
	             && child->job_stopped && !child->job_stop_reported;
	int jcont  = !zombie && !ptrace && !jstop && (flags & WCONTINUED) && child->job_continued;
	int ev = zombie ? 1 : (ptrace ? 2 : (jstop ? 3 : (jcont ? 4 : 0)));
	if (zombie && !(flags & WEXITED)) ev = -1;
	if (ptrace && !keep) child->ptrace_stop_reported = 1;
	if (jstop && !keep)  child->job_stop_reported = 1;
	if (jcont && !keep)  child->job_continued = 0;
	if (ev > 0) wait_snap(child, ev, snap);
	/* A child in PROC_DEAD has exited and already passed its wakeup in
	 * task_exit; it only becomes PROC_ZOMBIE later in schedule(). Sleeping now
	 * would miss that wakeup forever — yield and retry until it is ZOMBIE. */
	if (!ev && will_sleep && child->state != PROC_DEAD) {
		parent->state = PROC_SLEEPING;
		parent->sleeping_on_queue = 1;  /* woken by task_exit, not the per-tick poll */
		parent->in_wait = 1;
	}
	spin_unlock_irq(&sched_lock, f);
	return ev;
}

void proc_reap(process_t *child)
{
	KASSERT(child->magic == PROC_MAGIC);
	pid_t cpid = child->pid;

	uint64_t f = spin_lock_irq(&sched_lock);
	if (cpid >= 0 && cpid < proc_cnt)
		proctab[cpid] = NULL;

	process_t **p = &proctab[0]->next;
	while (*p && *p != child)
		p = &(*p)->next;
	if (*p == child)
		*p = child->next;
	/* times(2): a reaped child's CPU time (and its own reaped children's) rolls
	 * up into the parent's cutime/cstime. */
	pid_t up = child->pid == child->tgid ? child->ppid : child->tgid;   /* a thread's time is its group's */
	process_t *par = (up >= 0 && up < proc_cnt) ? proctab[up] : NULL;
	if (par) {
		par->cutime_ticks += child->utime_ticks + child->cutime_ticks;
		par->cstime_ticks += child->stime_ticks + child->cstime_ticks;
	}
	/* defer the PCB free if a /proc reader still holds a proc_get pin */
	child->pcb_dead = 1;
	int free_now = (child->refcount == 0);
	spin_unlock_irq(&sched_lock, f);

	/*
	 * kstack was already freed by sched_free_dying_kstack() (called from
	 * assembly after the stack switch), and schedule() cleared child->kstack.
	 * proc_find_zombie only returns PROC_ZOMBIE processes, meaning schedule()
	 * has completed — no CPU has child as cpu->current anymore.
	 */
	if (child->kstack)
		buddy_free(VIRT_TO_PHYS(child->kstack), kstack_order_val);
	if (free_now) {
		cpu_affinity_free(child);
		proc_cmdline_free(child);
		child->magic = 0;   /* poison before free: trips a later use-after-free */
		buddy_free(VIRT_TO_PHYS(child), PCB_ORDER);
	}
}
