/* ── process lifecycle ────────────────────────────────────────────────────────
 * Creating processes (proc_create / proc_exec for kernel tasks, proc_fork /
 * proc_vfork / proc_clone_thread for the clone family), tearing them down
 * (task_exit), and reaping (proc_find_zombie, the wait helpers, proc_reap).
 * Looking up a process by pid lives here too. The scheduling mechanics these
 * lean on — runqueues, alloc_pid, schedule() — stay in sched.c.
 *
 * Compiled as part of sched.c (textual include): the scheduler statics
 * (proctab, sched_lock, alloc_pid, runqueue_add, kstack_order_val) are in scope.
 * ───────────────────────────────────────────────────────────────────────── */

static spinlock_t sighand_lock;

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
	process_t *p = (process_t *)PHYS_TO_VIRT(buddy_alloc(0));
	if (!p) return -1;
	memset(p, 0, sizeof(*p));
	p->sighand = sighand_alloc(NULL);
	if (!p->sighand) { buddy_free(VIRT_TO_PHYS(p), 0); return -1; }
	p->magic = PROC_MAGIC;

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
	pidns_attach(p, &init_pid_ns, pid);
	spin_unlock_irq(&sched_lock, f);
	return p->pid;
}

pid_t proc_exec(const char *name, const void *elf_data, size_t elf_len)
{
	process_t *p = (process_t *)PHYS_TO_VIRT(buddy_alloc(0));
	if (!p) return -1;
	memset(p, 0, sizeof(*p));
	p->magic = PROC_MAGIC;
	p->sighand = sighand_alloc(NULL);
	if (!p->sighand) { buddy_free(VIRT_TO_PHYS(p), 0); return -1; }

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

	/* name is the image's full path: comm is its basename, exe_path the whole
	 * path (glibc readlinks /proc/self/exe and asserts it is absolute). */
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

	p->vm = vm_create();
	if (!p->vm) goto fail;

	elf_info_t ei = {0};
	if (elf_load(p->vm, elf_data, elf_len, 0, &ei) != 0) {
		kprintf("exec: elf_load failed\n");
		goto fail;
	}

	p->brk          = ei.load_end;
	p->mmap_next    = ARCH_USER_MMAP_BASE;
	p->huge1g_next  = HUGE1G_VA_BASE;
	p->vfork_parent = -1;
	p->reaped       = 0;

	/* Dynamically linked init: load its interpreter (ld.so) and enter that
	 * instead — the loader relocates the image and jumps to the real entry,
	 * which it reads from AT_ENTRY. Same contract execve(2) honours. */
	uint64_t jump_entry = ei.entry;
	uint64_t at_base    = 0;
	uint64_t interp_lo = 0, interp_hi = 0;
	if (ei.interp[0]) {
		vfs_stat_t ist = {0};
		if (vfs_stat(ei.interp, &ist) == 0 && ist.size > 0) {
			uint8_t *ibuf = kmalloc(ist.size);
			if (ibuf) {
				int ifd = vfs_open(ei.interp, O_RDONLY, 0);
				if (ifd >= 0) {
					vfs_read(ifd, ibuf, ist.size);
					vfs_close(ifd);
					elf_info_t ii = {0};
					if (elf_load(p->vm, ibuf, (size_t)ist.size, p->mmap_next, &ii) == 0) {
						jump_entry   = ii.entry;
						at_base      = p->mmap_next;
						interp_lo    = ii.load_bias;
						interp_hi    = ii.load_end;
						p->mmap_next = (ii.load_end + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
					}
				}
				kfree(ibuf);
			}
		}
	}

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
	arch_vm_switch((uint64_t)p->vm->root, p->vm->asid);

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
	sp -= 16; ((uint64_t*)sp)[0] = 7; ((uint64_t*)sp)[1] = at_base; /* AT_BASE */
	sp -= 16; ((uint64_t*)sp)[0] = 6; ((uint64_t*)sp)[1] = PAGE_SIZE; /* AT_PAGESZ */
	sp -= 16; ((uint64_t*)sp)[0] = 5; ((uint64_t*)sp)[1] = ei.phnum; /* AT_PHNUM */
	sp -= 16; ((uint64_t*)sp)[0] = 4; ((uint64_t*)sp)[1] = ei.phentsize; /* AT_PHENT */
	sp -= 16; ((uint64_t*)sp)[0] = 3; ((uint64_t*)sp)[1] = ei.phdr_va; /* AT_PHDR */
	sp -= 16; ((uint64_t*)sp)[0] = 25; ((uint64_t*)sp)[1] = at_random; /* AT_RANDOM */

	sp -= 8; *(uint64_t*)sp = 0; /* envp NULL */
	sp -= 8; *(uint64_t*)sp = 0; /* argv NULL */
	sp -= 8; *(uint64_t*)sp = at_execfn; /* argv[0] */
	sp -= 8; *(uint64_t*)sp = 1; /* argc */

	arch_context_init_user(ctx, jump_entry, sp);

	if (old_vm) arch_vm_switch((uint64_t)old_vm->root, old_vm->asid);
	else arch_vm_switch((uint64_t)kernel_vm_space->root, 0);

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
	/* pid 1 is handed fd 0/1/2 = /dev/console by the kernel, as Linux does — a
	 * freestanding C runtime (glibc) assumes the standard descriptors already
	 * exist and aborts otherwise. */
	if (p->fds) {
		vfs_open_fds(p->fds, p->euid, p->egid, p->umask, "/dev/console", O_RDONLY, 0);
		vfs_open_fds(p->fds, p->euid, p->egid, p->umask, "/dev/console", O_WRONLY, 0);
		vfs_open_fds(p->fds, p->euid, p->egid, p->umask, "/dev/console", O_WRONLY, 0);
	}
	/* Inherit the cwd, but guarantee it is absolute: init's parent is the boot
	 * task, whose cwd was never set — an empty/relative cwd makes getcwd(2) fail
	 * (musl rejects a non-'/' result), which then propagates to every process. */
	if (current->cwd[0] == '/') memcpy(p->cwd, current->cwd, 256);
	else { p->cwd[0] = '/'; p->cwd[1] = '\0'; }
	p->umask = current->umask;
	p->root  = current->root;   /* chroot survives exec */

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
		pidns_attach(p, &init_pid_ns, pid);
		spin_unlock_irq(&sched_lock, f);
	}
	return p->pid;

fail:
	if (p->vm) vm_destroy(p->vm);
	if (p->fds) fd_table_destroy(p->fds);
	sighand_put(p->sighand);
	if (p->kstack) buddy_free(VIRT_TO_PHYS(p->kstack), kstack_order_val);
	buddy_free(VIRT_TO_PHYS(p), 0);
	return -1;
}

pid_t proc_fork(uint64_t clone_flags, uint64_t newsp)
{
	process_t *parent = task_current();
	arch_save_tls(parent);

	process_t *child = (process_t *)PHYS_TO_VIRT(buddy_alloc(0));
	if (!child) return -1;
	memset(child, 0, sizeof(*child));
	child->magic = PROC_MAGIC;

	/* cgroup pids limit: reject the fork if it would exceed pids.max. Charged
	 * here; released by cgroup_cancel_fork on a later failure, or by
	 * cgroup_exit when the child eventually exits. */
	if (cgroup_can_fork(parent) != 0) { buddy_free(VIRT_TO_PHYS(child), 0); return -11; }
	cgroup_fork_attach(child, parent);

	/* pick the child's pid namespace: a fresh one for clone(CLONE_NEWPID), the
	 * parent's pending ns (unshare CLONE_NEWPID), else the parent's own. */
	pid_namespace_t *target_pidns;
	int owns_pidns = 0;
	if (clone_flags & CLONE_NEWPID) {
		target_pidns = pid_ns_create_child(parent->pid_ns ? parent->pid_ns : &init_pid_ns);
		if (!target_pidns) { cgroup_cancel_fork(parent); buddy_free(VIRT_TO_PHYS(child), 0); return -12; }
		owns_pidns = 1;
	} else {
		target_pidns = parent->pid_ns_new ? parent->pid_ns_new
		             : (parent->pid_ns ? parent->pid_ns : &init_pid_ns);
	}

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
	child->huge1g_next = parent->huge1g_next;
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
	child->root         = parent->root;
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

	child->sighand = sighand_alloc(parent->sighand);   /* fork → private copy */
	if (!child->sighand) goto fail_early;
	child->sig_mask = parent->sig_mask;

	child->kstack = kstack_alloc();
	if (!child->kstack) goto fail_early;

	{
		uint8_t *ksp = child->kstack + kstack_size_val;
		arch_context_t *cctx = (arch_context_t *)(ksp - sizeof(arch_context_t));
		memcpy(cctx, parent->ctx, sizeof(arch_context_t));
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
	child->tls_base        = parent->tls_base;
	if (clone_flags & CLONE_NEWUTS) {
		child->uts_ns = uts_ns_clone(parent->uts_ns);
		if (!child->uts_ns) goto fail_early;
	} else {
		child->uts_ns = uts_ns_get(parent->uts_ns);
	}
	if (clone_flags & CLONE_NEWIPC) {
		child->ipc_ns = ipc_ns_clone(parent->ipc_ns);
		if (!child->ipc_ns) goto fail_early;
	} else {
		child->ipc_ns = ipc_ns_get(parent->ipc_ns);
	}
	/* a pending time ns (from the parent's unshare) applies to its children */
	child->time_ns = parent->time_ns_new
	               ? time_ns_get(parent->time_ns_new)
	               : time_ns_get(parent->time_ns);
	child->mnt_ns  = mnt_ns_get(parent->mnt_ns);
	if (clone_flags & CLONE_NEWUSER) {
		child->user_ns = user_ns_create_child(parent->user_ns ? parent->user_ns : &init_user_ns, parent->euid);
		if (!child->user_ns) goto fail_early;
		child->cap_effective = child->cap_permitted = CAP_FULL;
	} else {
		child->user_ns = user_ns_get(parent->user_ns);
	}
	if (clone_flags & CLONE_NEWNET) {
		child->net_ns = net_ns_create();
		if (!child->net_ns) goto fail_early;
	} else {
		child->net_ns = net_ns_get(parent->net_ns);
	}
	child->cgroup_ns = (clone_flags & CLONE_NEWCGROUP)
	                 ? cgroup_ns_create()
	                 : cgroup_ns_get(parent->cgroup_ns);
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
		pidns_attach(child, target_pidns, pid);
		spin_unlock_irq(&sched_lock, f);
	}
	if (owns_pidns) pid_ns_put(target_pidns);   /* child holds its own ref now */
	return child->pid;

fail_early:
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
	if (owns_pidns) pid_ns_put(target_pidns);
	cgroup_cancel_fork(parent);
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
	child->magic = PROC_MAGIC;

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
	child->root         = parent->root;
	child->umask        = parent->umask;
	child->sighand      = sighand_alloc(parent->sighand);
	if (!child->sighand) { buddy_free(VIRT_TO_PHYS(child), 0); return -1; }
	child->sig_mask     = parent->sig_mask;

	child->kstack = kstack_alloc();
	if (!child->kstack) {
		sighand_put(child->sighand);
		buddy_free(VIRT_TO_PHYS(child), 0);
		return -1;
	}

	uint8_t *ksp = child->kstack + kstack_size_val;
	arch_context_t *cctx = (arch_context_t *)(ksp - sizeof(arch_context_t));
	memcpy(cctx, parent->ctx, sizeof(arch_context_t));
	CTX_SYSCALL_RET(cctx, 0);
	child->ctx      = cctx;
	child->tls_base = parent->tls_base;
	child->uts_ns   = uts_ns_get(parent->uts_ns);
	child->ipc_ns   = ipc_ns_get(parent->ipc_ns);
	child->time_ns  = time_ns_get(parent->time_ns);
	child->mnt_ns   = mnt_ns_get(parent->mnt_ns);
	child->user_ns  = user_ns_get(parent->user_ns);
	child->net_ns   = net_ns_get(parent->net_ns);
	child->cgroup_ns = cgroup_ns_get(parent->cgroup_ns);

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
	pidns_attach(child, parent->pid_ns ? parent->pid_ns : &init_pid_ns, pid);

	if (parent->ctx)
		CTX_SYSCALL_RET(parent->ctx, child->pid);

	parent->state = PROC_SLEEPING;
	parent->sleeping_on_queue = 1;  /* woken by proc_vfork_exec_notify (exec) or task_exit (exit) */
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

pid_t proc_clone_thread(uint64_t newsp, uint64_t tls, uint32_t *ctid, uint32_t *ptid, uint32_t *set_tid)
{
	process_t *parent = task_current();
	process_t *child  = (process_t *)PHYS_TO_VIRT(buddy_alloc(0));
	if (!child) return -1;
	memset(child, 0, sizeof(*child));
	child->magic = PROC_MAGIC;

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
	child->root  = parent->root;
	child->umask = parent->umask;

	/* Threads share signal dispositions (CLONE_SIGHAND) but each has its own
	 * blocked mask, inherited from the creator. */
	child->sighand  = sighand_get(parent->sighand);
	child->sig_mask = parent->sig_mask;

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
	child->uts_ns   = uts_ns_get(parent->uts_ns);
	child->ipc_ns   = ipc_ns_get(parent->ipc_ns);
	child->time_ns  = time_ns_get(parent->time_ns);
	child->mnt_ns   = mnt_ns_get(parent->mnt_ns);
	child->user_ns  = user_ns_get(parent->user_ns);
	child->net_ns   = net_ns_get(parent->net_ns);
	child->cgroup_ns = cgroup_ns_get(parent->cgroup_ns);

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
	pidns_attach(child, parent->pid_ns ? parent->pid_ns : &init_pid_ns, pid);
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
	if (ptid)    *ptid    = (uint32_t)pid;
	if (set_tid) *set_tid = (uint32_t)pid;
	/* Publish the tid (and all child setup) to memory before the child can
	 * be observed runnable on another CPU. On weakly-ordered aarch64 a stale
	 * tid=0 read makes musl's __tl_lock a no-op and corrupts the thread list. */
	smp_mb();
	runqueue_add(child);

	spin_unlock_irq(&sched_lock, f);
	return child->pid;

fail:
	if (child->vm) vm_destroy(child->vm);
	sighand_put(child->sighand);
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

/* Count live threads sharing a thread group (for /proc/<pid>/status Threads). */
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
		poll_block((uint64_t)-1);
	}
}

/*
 * Called from assembly after the stack switch (mov sp, x0) in irq_handler and
 * .yield.  At that point we are on the NEW process's kstack, so it is safe to
 * release resources belonging to the process we just switched away from.
 */

void task_exit(void)
{
	process_t *p = task_current();
	KASSERT(p->magic == PROC_MAGIC);
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
	extern void vfs_posix_drop_pid(int pid);
	vfs_posix_drop_pid((int)p->pid);   /* release this process's fcntl record locks */

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
		*cctid = 0;
		smp_mb();
		arch_get_cpu_ctx()->wd_where = "exit:futex";
		futex_wake_addr(cctid);
	}

	uts_ns_put(p->uts_ns);
	p->uts_ns = NULL;
	ipc_ns_put(p->ipc_ns);
	p->ipc_ns = NULL;
	time_ns_put(p->time_ns);
	p->time_ns = NULL;
	if (p->time_ns_new) { time_ns_put(p->time_ns_new); p->time_ns_new = NULL; }
	mnt_ns_put(p->mnt_ns);
	p->mnt_ns = NULL;
	user_ns_put(p->user_ns);
	p->user_ns = NULL;
	net_ns_put(p->net_ns);
	p->net_ns = NULL;
	cgroup_ns_put(p->cgroup_ns);
	p->cgroup_ns = NULL;
	if (p->mem_charged) { cgroup_uncharge_mem(p->cgroup, p->mem_charged); p->mem_charged = 0; }
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
	cgroup_exit(p);

	uint64_t f = spin_lock_irq(&sched_lock);
	vm_space_t *dying_vm  = p->vm;   p->vm  = NULL;
	fd_table_t *dying_fds = p->fds;  p->fds = NULL;
	sighand_t  *dying_sh  = p->sighand; p->sighand = NULL;
	spin_unlock_irq(&sched_lock, f);

	/* Free resources — process is still schedulable during this. */
	sighand_put(dying_sh);
	arch_get_cpu_ctx()->wd_where = "exit:fdput";
	if (dying_fds) fd_table_put(dying_fds);
	if (dying_vm) {
		arch_get_cpu_ctx()->wd_where = "exit:vmdestroy";
		vm_activate();
		vm_destroy(dying_vm);
	}
	arch_get_cpu_ctx()->wd_where = "exit:vmas";
	vma_t *v = p->vmas; p->vmas = NULL;
	while (v) { vma_t *nx = v->next; kfree(v); v = nx; }

	/* Cleanup done — now mark dead and wake parent. */
	arch_get_cpu_ctx()->wd_where = "exit:reparent";
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
	int walk = 0, walk_max = proc_cnt + 8;
	for (process_t *c = sentinel2->next; c && c != sentinel2; c = c->next) {
		if (++walk > walk_max)
			kpanic("task_exit: process list cycle (corrupt ->next)");
		if (c->ppid == (pid_t)p->pid && c != p) {
			/* orphans reparent to the reaper (nr 1) of their own pid ns */
			pid_namespace_t *cns = c->pid_ns;
			process_t *reaper = (cns && cns != &init_pid_ns && cns->reaper && cns->reaper != c)
			                  ? cns->reaper : init_proc;
			c->ppid = reaper ? (pid_t)reaper->pid : 0;
			if ((c->state == PROC_DEAD || c->state == PROC_ZOMBIE) && reaper)
				reaper->sig_pending |= (1UL << (SIGCHLD - 1));
		}
	}

	/* A pid namespace's init dying takes the whole namespace down: SIGKILL
	 * every remaining member (standard CLONE_NEWPID semantics). */
	if (exit_is_reaper) {
		exit_pidns->dead = 1;
		for (process_t *c = sentinel2->next; c && c != sentinel2; c = c->next)
			if (c != p && c->pid_ns == exit_pidns) {
				c->sig_pending |= (1UL << (SIGKILL - 1));
				if (c->state == PROC_SLEEPING) {
					c->state = PROC_READY;
					if (!c->on_cpu) runqueue_add(c);
					arch_get_cpu_ctx()->need_resched = 1;
				}
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

	if (exit_is_reaper) pid_ns_put(exit_pidns);   /* drop the teardown ref */

	arch_yield();
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
	int dead_pending = 0;
	for (process_t *c = sentinel->next; c && c != sentinel; c = c->next) {
		if (c->ppid == ppid) {
			if (c->state == PROC_ZOMBIE && !c->reaped) {
				c->reaped = 1;
				*out_child = c;
				break;
			}
			if (c->state == PROC_DEAD)
				dead_pending = 1;   /* exiting; becomes ZOMBIE shortly */
			if (c->state != PROC_ZOMBIE)
				*out_any_alive = 1;
		}
	}
	/* Don't sleep while a child is mid-exit (PROC_DEAD): its task_exit wakeup
	 * has already fired, and it only turns ZOMBIE later in schedule(). Yield
	 * and retry instead, or the wakeup is lost forever. */
	if (!*out_child && *out_any_alive && will_sleep && !dead_pending) {
		parent->state = PROC_SLEEPING;
		parent->sleeping_on_queue = 1;  /* woken by task_exit, not the per-tick poll */
	}
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
	/* A child in PROC_DEAD has exited and already passed its wakeup in
	 * task_exit; it only becomes PROC_ZOMBIE later in schedule(). Sleeping now
	 * would miss that wakeup forever — yield and retry until it is ZOMBIE. */
	if (!zombie && !ptrace && will_sleep && child->state != PROC_DEAD) {
		parent->state = PROC_SLEEPING;
		parent->sleeping_on_queue = 1;  /* woken by task_exit, not the per-tick poll */
	}
	spin_unlock_irq(&sched_lock, f);
	return zombie ? 1 : (ptrace ? 2 : 0);
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
	spin_unlock_irq(&sched_lock, f);

	/*
	 * kstack was already freed by sched_free_dying_kstack() (called from
	 * assembly after the stack switch), and schedule() cleared child->kstack.
	 * proc_find_zombie only returns PROC_ZOMBIE processes, meaning schedule()
	 * has completed — no CPU has child as cpu->current anymore.
	 */
	if (child->kstack)
		buddy_free(VIRT_TO_PHYS(child->kstack), kstack_order_val);
	child->magic = 0;   /* poison before free: trips a later use-after-free */
	buddy_free(VIRT_TO_PHYS(child), 0);
}
