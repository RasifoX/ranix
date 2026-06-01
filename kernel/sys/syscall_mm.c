/* ── memory-management syscalls ──────────────────────────────────────────────
 * The user side of the virtual-memory layer: growing the heap (brk), mapping
 * and unmapping regions (mmap/munmap), and changing their protection
 * (mprotect). Each mapped region is tracked by a VMA (see vma_add in syscall.c)
 * so the page-fault handler knows a region's intent — anonymous vs file-backed,
 * shared vs private, and which permissions it carries. madvise/msync are
 * accepted as no-ops: we have no page cache to flush or readahead to tune.
 *
 * Compiled as part of syscall.c (textual include), so the static helpers it
 * relies on — vma_add and the PROT_, MAP_ and VMA_ flag macros — are in scope.
 * ───────────────────────────────────────────────────────────────────────── */

/* Grow or shrink the heap. addr==0 queries the current break. */
static long sys_brk(uint64_t addr)
{
	process_t *p = task_current();
	if (addr == 0) return (long)p->brk;

	uint64_t new_brk = (addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

	if (new_brk < p->brk) {
		/* Shrink: free pages above new_brk */
		for (uint64_t va = new_brk; va < p->brk; va += PAGE_SIZE)
			vm_unmap(p->vm, va);
		p->brk = new_brk;
		return (long)new_brk;
	}

	for (uint64_t va = p->brk; va < new_brk; va += PAGE_SIZE) {
		void *page = buddy_alloc(0);
		if (!page) { kprintf("brk: ENOMEM pid=%u brk=%lx new=%lx free=%lu\n", p->pid, p->brk, new_brk, pmm_free_pages()); return (long)p->brk; }
		memset((void *)PHYS_TO_VIRT(page), 0, PAGE_SIZE);
		vm_map(p->vm, va, (uint64_t)page, VM_USER | VM_READ | VM_WRITE);
	}
	p->brk = new_brk;
	return (long)new_brk;
}

/* Map an anonymous or file-backed region. Anonymous pages are zero-filled on
 * demand here; file-backed pages are read in a page at a time. The resulting
 * region is recorded as a VMA carrying the requested protection. */
static long sys_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, uint64_t off)
{
	if (len == 0) return -22;
	process_t *p = task_current();
	len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

	uint64_t va;
	if ((flags & MAP_FIXED) && addr) va = addr & ~(uint64_t)(PAGE_SIZE - 1);
	else { va = p->mmap_next; p->mmap_next += len; }
	uint32_t vmflags = VM_USER;
	if (prot & PROT_READ)  vmflags |= VM_READ;
	if (prot & PROT_WRITE) vmflags |= VM_WRITE;
	if (prot & PROT_EXEC)  vmflags |= VM_EXEC;

	if (flags & MAP_ANONYMOUS) {
		if (prot == 0) {
			/* PROT_NONE: make pages inaccessible by unmapping them */
			for (uint64_t v = va; v < va + len; v += PAGE_SIZE)
				vm_unmap(p->vm, v);
		} else {
			if (cgroup_charge_mem(p->cgroup, (long)len) != 0) return -12; /* over memory.max */
			for (uint64_t v = va; v < va + len; v += PAGE_SIZE) {
				void *pg = buddy_alloc(0);
				if (!pg) { cgroup_uncharge_mem(p->cgroup, (long)len); kprintf("mmap: ENOMEM pid=%u va=%lx len=%zu free=%lu\n", p->pid, va, len, pmm_free_pages()); return -12; }
				memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
				vm_map(p->vm, v, (uint64_t)pg, vmflags);
			}
			p->mem_charged += (long)len;
		}
		{
			uint32_t vf = VMA_USER | VMA_ANON;
			if (prot & PROT_READ)  vf |= VMA_READ;
			if (prot & PROT_WRITE) vf |= VMA_WRITE;
			if (prot & PROT_EXEC)  vf |= VMA_EXEC;
			if (flags & MAP_SHARED) vf |= VMA_SHARED;
			vma_add(p, va, len, vf);
		}
		return (long)va;
	}

	/* File-backed mmap */
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -9;

	/* If it's a device mapping via mmap callback */
	if (f->inode->fops && f->inode->fops->mmap) {
		uint64_t file_off = off;
		uint32_t dev_vmflags = vmflags | VM_NOCACHE;
		for (uint64_t v = va; v < va + len; v += PAGE_SIZE) {
			uint64_t phys = f->inode->fops->mmap(f, (off_t)file_off);
			if (!phys) {
				/* Mapping failed, cleanup is complex here, but we return error */
				return -22; /* EINVAL */
			}
			vm_map(p->vm, v, phys, dev_vmflags);
			file_off += PAGE_SIZE;
		}
		{
			uint32_t vf = VMA_USER;
			if (prot & PROT_READ)  vf |= VMA_READ;
			if (prot & PROT_WRITE) vf |= VMA_WRITE;
			if (prot & PROT_EXEC)  vf |= VMA_EXEC;
			vma_add(p, va, len, vf);
		}
		return (long)va;
	}

	if (!f->inode->fops->read) return -9;

	uint64_t file_off = off;
	for (uint64_t v = va; v < va + len; v += PAGE_SIZE) {
		void *pg = buddy_alloc(0);
		if (!pg) return -12;
		void *pg_virt = (void *)PHYS_TO_VIRT(pg);
		memset(pg_virt, 0, PAGE_SIZE);

		/* read one page from file at current offset via a temporary file handle */
		vfs_file_t tmp_f = *f;
		tmp_f.pos = (off_t)file_off;
		ssize_t nr = f->inode->fops->read(&tmp_f, pg_virt, PAGE_SIZE);
		(void)nr;
		file_off += PAGE_SIZE;

		vm_map(p->vm, v, (uint64_t)pg, vmflags);

#if defined(__aarch64__)
		if (prot & PROT_EXEC) {
			uintptr_t pp = (uintptr_t)pg_virt;
			for (uintptr_t l = pp; l < pp + PAGE_SIZE; l += 64)
				__asm__ volatile("dc cvac, %0" :: "r"(l) : "memory");
			__asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");
		}
#endif
	}
	{
		uint32_t vf = VMA_USER;
		if (prot & PROT_READ)  vf |= VMA_READ;
		if (prot & PROT_WRITE) vf |= VMA_WRITE;
		if (prot & PROT_EXEC)  vf |= VMA_EXEC;
		vma_add(p, va, len, vf);
	}
	return (long)va;
}

/* Unmap a range and drop any VMA fully contained in it. */
static long sys_munmap(uint64_t addr, size_t len)
{
	process_t *p  = task_current();
	addr          = addr & ~(PAGE_SIZE - 1);
	len           = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE)
		vm_unmap(p->vm, va);
	/* remove fully-covered VMAs */
	vma_t **pp = &p->vmas;
	while (*pp) {
		vma_t *v = *pp;
		if (v->vaddr >= addr && v->vaddr + v->size <= addr + len) {
			*pp = v->next;
			kfree(v);
		} else {
			pp = &v->next;
		}
	}
	return 0;
}

/* Change a range's protection. The PTEs are remapped and the covering VMAs'
 * flags are updated so the fault handler enforces the new protection (a write
 * to a now read-only page must fault). */
static long sys_mprotect(uint64_t addr, size_t len, int prot)
{
	if (len == 0) return 0;
	process_t *p = task_current();
	addr &= ~(uint64_t)(PAGE_SIZE - 1);
	len  = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

	uint32_t vmflags = VM_USER;
	if (prot & PROT_READ)  vmflags |= VM_READ;
	if (prot & PROT_WRITE) vmflags |= VM_WRITE;
	if (prot & PROT_EXEC)  vmflags |= VM_EXEC;

	extern uint64_t arch_vm_get_paddr(uint64_t *root, uint64_t vaddr);
	for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE) {
		uint64_t pa = arch_vm_get_paddr(p->vm->root, va);
		if (pa) {
			if (prot == 0)
				vm_unmap(p->vm, va);
			else
				vm_map(p->vm, va, pa, vmflags);
		} else if (prot != 0) {
			void *pg = buddy_alloc(0);
			if (!pg) return -12;
			memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
			vm_map(p->vm, va, (uint64_t)pg, vmflags);
		}
	}

	/* Keep VMA permissions in sync so the fault handler enforces the new prot.
	 * Split any VMA that the range only partially covers, so the new prot
	 * applies to exactly [addr, addr+len) — applying it to the whole VMA would
	 * e.g. strip write from a binary's data segment when ld.so makes its small
	 * RELRO sub-region read-only. */
	uint32_t vmaflags = VMA_USER;
	if (prot & PROT_READ)  vmaflags |= VMA_READ;
	if (prot & PROT_WRITE) vmaflags |= VMA_WRITE;
	if (prot & PROT_EXEC)  vmaflags |= VMA_EXEC;
	for (vma_t *v = p->vmas; v; v = v->next) {
		uint64_t vstart = v->vaddr, vend = v->vaddr + v->size;
		if (vend <= addr || vstart >= addr + len) continue; /* no overlap */
		uint64_t lo = addr > vstart ? addr : vstart;
		uint64_t hi = (addr + len) < vend ? (addr + len) : vend;
		uint32_t newf = vmaflags | (v->flags & (VMA_SHARED | VMA_ANON));
		if (vstart == lo && vend == hi) {
			v->flags = newf;                       /* whole VMA */
		} else {
			uint32_t oldf = v->flags;              /* carve out the middle */
			if (vstart < lo) vma_add(p, vstart, lo - vstart, oldf);
			if (hi < vend)   vma_add(p, hi, vend - hi, oldf);
			v->vaddr = lo; v->size = hi - lo; v->flags = newf;
		}
	}

	return 0;
}

/* No page cache to advise or flush — accept and ignore. */
static long sys_madvise(uint64_t addr, size_t len, int advice)
{
	(void)addr; (void)len; (void)advice;
	return 0;
}

static long sys_msync(uint64_t addr, size_t len, int flags)
{
	(void)addr; (void)len; (void)flags;
	return 0;
}
