/* ── memory-management syscalls ──────────────────────────────────────────────
 * The user side of the virtual-memory layer: growing the heap (brk), mapping
 * and unmapping regions (mmap/munmap), and changing their protection
 * (mprotect). Each mapped region is tracked by a VMA (see vma_add in syscall.c)
 * so the page-fault handler knows a region's intent — anonymous vs file-backed,
 * shared vs private, and which permissions it carries. madvise acts on the hints
 * that matter without a page cache (prefault, drop, lazy-free, hugepage); msync
 * is a no-op as there is nothing to write back. mlock prefaults and pins a range.
 *
 * Compiled as part of syscall.c (textual include), so the static helpers it
 * relies on — vma_add and the PROT_, MAP_ and VMA_ flag macros — are in scope.
 * ───────────────────────────────────────────────────────────────────────── */

/* Order-0 user page with OOM fallback: on a dry buddy, kill the biggest user
 * process and retry so a growth request can be satisfied from the reclaimed
 * memory instead of failing outright. Bounded — a kill freeing nothing
 * (the requester itself is the victim, dying only on syscall return) falls
 * through to ENOMEM rather than spinning. */
static void *mm_alloc_page_oom(void)
{
	void *pg = buddy_alloc(0);
	for (int i = 0; !pg && i < 8; i++) {
		if (oom_kill_alloc() != OOM_KILLED_OTHER) break;
		pg = buddy_alloc(0);
	}
	return pg;
}

/* Grow or shrink the heap. addr==0 queries the current break. */
static long sys_brk(uint64_t addr)
{
	process_t *p = task_current();
	if (addr == 0) return (long)p->brk;
	if (!access_ok((void *)addr, 1)) return (long)p->brk;   /* reject kernel-range brk */

	uint64_t new_brk = (addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

	if (new_brk < p->brk) {
		/* Shrink: free pages above new_brk */
		for (uint64_t va = new_brk; va < p->brk; va += PAGE_SIZE)
			vm_unmap(p->vm, va);
		p->brk = new_brk;
		return (long)new_brk;
	}

	for (uint64_t va = p->brk; va < new_brk; va += PAGE_SIZE) {
		void *page = mm_alloc_page_oom();
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

	/* MAP_FIXED maps at a user-chosen address; reject a kernel-range target so it
	 * cannot write a PTE over the (shared) kernel half of the page tables. */
	if ((flags & MAP_FIXED) && addr && !access_ok((void *)addr, len)) return -22;

	uint32_t vmflags = VM_USER;
	if (prot & PROT_READ)  vmflags |= VM_READ;
	if (prot & PROT_WRITE) vmflags |= VM_WRITE;
	if (prot & PROT_EXEC)  vmflags |= VM_EXEC;

	/* MAP_HUGETLB | MAP_HUGE_1GB: back the region with 1 GB blocks from the
	 * reserved pool, in the process's own high VA window. Eagerly populated. */
	if ((flags & MAP_HUGETLB) && ((flags >> MAP_HUGE_SHIFT) & 0x3F) == 30 &&
	    (flags & MAP_ANONYMOUS) && prot != 0) {
		len = (len + HUGE_1G_SIZE - 1) & HUGE_1G_MASK;
		uint64_t va;
		if ((flags & MAP_FIXED) && addr) va = addr & HUGE_1G_MASK;
		else {
			p->huge1g_next = (p->huge1g_next + HUGE_1G_SIZE - 1) & HUGE_1G_MASK;
			va = p->huge1g_next; p->huge1g_next += len;
		}
		if (cgroup_charge_mem(p->cgroup, (long)len) != 0) { cgroup_oom_kill(p->cgroup); return -12; }
		for (uint64_t v = va; v < va + len; v += HUGE_1G_SIZE) {
			uint64_t blk = huge1g_alloc();
			if (!blk || vm_map_huge_1g(p->vm, v, blk, vmflags) != 0) {
				if (blk) huge1g_put(blk);
				for (uint64_t u = va; u < v; u += HUGE_1G_SIZE) vm_unmap(p->vm, u);
				cgroup_uncharge_mem(p->cgroup, (long)len);
				return -12;
			}
			memset((void *)PHYS_TO_VIRT(blk), 0, HUGE_1G_SIZE);
		}
		p->mem_charged += (long)len;
		{
			uint32_t vf = VMA_USER | VMA_ANON | VMA_HUGE | VMA_HUGE_1G;
			if (prot & PROT_READ)  vf |= VMA_READ;
			if (prot & PROT_WRITE) vf |= VMA_WRITE;
			if (prot & PROT_EXEC)  vf |= VMA_EXEC;
			if (flags & MAP_SHARED) vf |= VMA_SHARED;
			vma_clip(p, va, len); vma_add(p, va, len, vf);
		}
		return (long)va;
	}

	/* MAP_HUGETLB: back the region with 2 MB blocks. Both length and address
	 * round to the huge-page boundary; pages are populated eagerly so there is
	 * no demand-fault path to teach about blocks. */
	if ((flags & MAP_HUGETLB) && (flags & MAP_ANONYMOUS) && prot != 0) {
		len = (len + HUGE_PAGE_SIZE - 1) & HUGE_PAGE_MASK;
		uint64_t va;
		if ((flags & MAP_FIXED) && addr) va = addr & HUGE_PAGE_MASK;
		else {
			p->mmap_next = (p->mmap_next + HUGE_PAGE_SIZE - 1) & HUGE_PAGE_MASK;
			va = p->mmap_next; p->mmap_next += len;
		}
		if (cgroup_charge_mem(p->cgroup, (long)len) != 0) { cgroup_oom_kill(p->cgroup); return -12; }
		uint64_t done = 0;
		for (uint64_t v = va; v < va + len; v += HUGE_PAGE_SIZE) {
			void *blk = buddy_alloc(9);   /* 2 MB = 512 pages */
			if (!blk || vm_map_huge(p->vm, v, (uint64_t)blk, vmflags) != 0) {
				if (blk) buddy_free((uint64_t)blk, 9);
				for (uint64_t u = va; u < v; u += HUGE_PAGE_SIZE) vm_unmap(p->vm, u);
				cgroup_uncharge_mem(p->cgroup, (long)len);
				return -12;
			}
			memset((void *)PHYS_TO_VIRT(blk), 0, HUGE_PAGE_SIZE);
			done += HUGE_PAGE_SIZE;
		}
		(void)done;
		p->mem_charged += (long)len;
		{
			uint32_t vf = VMA_USER | VMA_ANON | VMA_HUGE;
			if (prot & PROT_READ)  vf |= VMA_READ;
			if (prot & PROT_WRITE) vf |= VMA_WRITE;
			if (prot & PROT_EXEC)  vf |= VMA_EXEC;
			if (flags & MAP_SHARED) vf |= VMA_SHARED;
			vma_clip(p, va, len); vma_add(p, va, len, vf);
		}
		return (long)va;
	}

	len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

	uint64_t va;
	if ((flags & MAP_FIXED) && addr) va = addr & ~(uint64_t)(PAGE_SIZE - 1);
	else {
		/* The bump-allocated mmap window ends at the stack; refuse to hand out a
		 * VA that would overlap it (Linux returns ENOMEM when no region fits)
		 * rather than silently map over the stack. */
		uint64_t ceil = ARCH_USER_STACK_TOP - (uint64_t)ARCH_USER_STACK_PAGES * PAGE_SIZE;
		if (p->mmap_next + len > ceil || p->mmap_next + len < p->mmap_next) return -12;
		va = p->mmap_next; p->mmap_next += len;
	}

	if (flags & MAP_ANONYMOUS) {
		if (prot == 0) {
			/* PROT_NONE: make pages inaccessible by unmapping them */
			for (uint64_t v = va; v < va + len; v += PAGE_SIZE)
				vm_unmap(p->vm, v);
		} else {
			if (cgroup_charge_mem(p->cgroup, (long)len) != 0) { cgroup_oom_kill(p->cgroup); return -12; } /* over memory.max */
			for (uint64_t v = va; v < va + len; v += PAGE_SIZE) {
				void *pg = mm_alloc_page_oom();
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
			if ((flags & MAP_LOCKED) || p->mlock_future) vf |= VMA_LOCKED;
			vma_clip(p, va, len); vma_add(p, va, len, vf);
		}
		return (long)va;
	}

	/* File-backed mmap */
	vfs_file_t *f = fd_get(p->fds, fd);
	if (!f) return -9;

	/* Mapping backed by an inode-owned page via the mmap fop. Two kinds:
	 *  - cacheable (ramfs/tmpfs): the fop returns a page of the file's own
	 *    write-back buffer. MAP_SHARED maps it directly so stores land in the
	 *    file and are seen by read() and other mappers; VM_NOFREE + the fop's
	 *    extra page ref keep the buffer alive across this mapping's unmap, and
	 *    the inode is pinned so a later grow cannot relocate it. MAP_PRIVATE
	 *    falls through to the copy path below for COW semantics.
	 *  - device/hugetlb: non-cacheable, no write-back. */
	if (f->inode->fops && f->inode->fops->mmap) {
		int cacheable = f->inode->fops->mmap_cacheable;
		if (!cacheable || (flags & MAP_SHARED)) {
			uint64_t file_off = off;
			uint32_t pte_flags = vmflags | (cacheable ? VM_NOFREE : VM_NOCACHE);
			for (uint64_t v = va; v < va + len; v += PAGE_SIZE) {
				uint64_t phys = f->inode->fops->mmap(f, (off_t)file_off);
				if (!phys) {
					for (uint64_t u = va; u < v; u += PAGE_SIZE) vm_unmap(p->vm, u);
					return -22; /* EINVAL — offset past backing */
				}
				vm_map(p->vm, v, phys, pte_flags);
				file_off += PAGE_SIZE;
			}
			if (cacheable) f->inode->mmap_pinned = 1;
			{
				uint32_t vf = VMA_USER;
				if (prot & PROT_READ)  vf |= VMA_READ;
				if (prot & PROT_WRITE) vf |= VMA_WRITE;
				if (prot & PROT_EXEC)  vf |= VMA_EXEC;
				if (cacheable && (flags & MAP_SHARED)) vf |= VMA_SHARED;
				vma_clip(p, va, len); vma_add(p, va, len, vf);
			}
			return (long)va;
		}
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
		vma_clip(p, va, len); vma_add(p, va, len, vf);
	}
	return (long)va;
}

/* Unmap a range and drop any VMA fully contained in it. */
static long sys_munmap(uint64_t addr, size_t len)
{
	process_t *p  = task_current();
	addr          = addr & ~(PAGE_SIZE - 1);
	len           = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	if (!access_ok((void *)addr, len)) return -22;   /* user range only */
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
	if (!access_ok((void *)addr, len)) return -22;   /* user range only */

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

	/* vm_map only flushed the local TLB; a sibling thread on another CPU keeps a
	 * stale (e.g. still-writable) entry for the reprotected range. Shoot them
	 * down so the new protection takes effect everywhere. */
	arch_tlb_flush_range(p->vm, addr, addr + len);

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

#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_FREE       8
#define MADV_HUGEPAGE   14
#define MADV_NOHUGEPAGE 15

/* Transparent hugepages: collapse a 2 MB-aligned span of sole-owned, fully
 * present anonymous 4K pages into one 2 MB block, preserving contents. Shared
 * (post-fork) or partly-unmapped spans are skipped. Reuses the 2 MB block
 * machinery, so the result COW-splits/unmaps/frees like any hugepage. */
static void thp_collapse(process_t *p, uint64_t va)
{
	vma_t *vma = NULL;
	for (vma_t *v = p->vmas; v; v = v->next)
		if (va >= v->vaddr && va + HUGE_PAGE_SIZE <= v->vaddr + v->size) { vma = v; break; }
	if (!vma) return;
	if (!(vma->flags & VMA_ANON) || !(vma->flags & VMA_WRITE) || (vma->flags & VMA_SHARED))
		return;

	extern uint64_t arch_vm_get_paddr(uint64_t *root, uint64_t vaddr);
	uint64_t pa0 = arch_vm_get_paddr(p->vm->root, va);
	if (!pa0) return;
	for (int j = 0; j < 512; j++) {
		uint64_t pa = arch_vm_get_paddr(p->vm->root, va + (uint64_t)j * PAGE_SIZE);
		if (!pa) return;
		page_t *pg = paddr_to_page(pa);
		if (!pg || pg->refcount != 1) return;   /* absent or shared → skip */
	}

	void *blk = buddy_alloc(9);   /* 2 MB */
	if (!blk) return;
	for (int j = 0; j < 512; j++) {
		uint64_t pa = arch_vm_get_paddr(p->vm->root, va + (uint64_t)j * PAGE_SIZE);
		memcpy((void *)PHYS_TO_VIRT((uint64_t)blk + (uint64_t)j * PAGE_SIZE),
		       (void *)PHYS_TO_VIRT(pa), PAGE_SIZE);
	}
	for (uint64_t v = va; v < va + HUGE_PAGE_SIZE; v += PAGE_SIZE)
		vm_unmap(p->vm, v);               /* frees the old 4K pages + empties the table */
	if (vm_map_huge(p->vm, va, (uint64_t)blk, VM_USER | VM_READ | VM_WRITE) != 0)
		buddy_free((uint64_t)blk, 9);
}

/* Locate the anonymous VMA covering [va, va+PAGE_SIZE). Huge VMAs are excluded:
 * they are eagerly populated and never carry a 4K demand-fault path, so the
 * per-page prefault/drop helpers must leave them alone. */
static vma_t *anon_vma_at(process_t *p, uint64_t va)
{
	for (vma_t *v = p->vmas; v; v = v->next) {
		if (va < v->vaddr || va >= v->vaddr + v->size) continue;
		if (!(v->flags & VMA_ANON) || (v->flags & (VMA_HUGE | VMA_HUGE_1G))) return NULL;
		return v;
	}
	return NULL;
}

/* Prefault every absent page of an anonymous range with a zero page carrying
 * the VMA's permissions — the work MADV_WILLNEED and mlock want done up front so
 * a later touch never faults. Best-effort: a dry buddy just stops early. */
static void mm_prefault(process_t *p, uint64_t addr, size_t len)
{
	extern uint64_t arch_vm_get_paddr(uint64_t *root, uint64_t vaddr);
	uint64_t start = addr & ~(uint64_t)(PAGE_SIZE - 1);
	uint64_t end   = (addr + len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	for (uint64_t va = start; va < end; va += PAGE_SIZE) {
		vma_t *v = anon_vma_at(p, va);
		if (!v || !(v->flags & VMA_READ)) continue;
		if (arch_vm_get_paddr(p->vm->root, va)) continue;
		if (cgroup_charge_mem(p->cgroup, PAGE_SIZE) != 0) return;
		void *pg = mm_alloc_page_oom();
		if (!pg) { cgroup_uncharge_mem(p->cgroup, PAGE_SIZE); return; }
		memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
		uint32_t mf = VM_USER | VM_READ;
		if (v->flags & VMA_WRITE) mf |= VM_WRITE;
		if (v->flags & VMA_EXEC)  mf |= VM_EXEC;
		if (vm_map(p->vm, va, (uint64_t)pg, mf) != 0) { buddy_free((uint64_t)pg, 0); cgroup_uncharge_mem(p->cgroup, PAGE_SIZE); return; }
		p->mem_charged += PAGE_SIZE;
	}
}

/* Drop the present pages of a private anonymous range (MADV_DONTNEED / MADV_FREE).
 * The pages are freed and the charge reversed; the next access demand-faults a
 * fresh zero page (vm_handle_fault). Shared anon is left intact — its pages back
 * other mappings — matching MADV_DONTNEED's no-op on MAP_SHARED. We lack reclaim,
 * so MADV_FREE drops eagerly rather than lazily; a re-read still reads as zero,
 * which is a permitted MADV_FREE outcome. */
static void mm_drop(process_t *p, uint64_t addr, size_t len)
{
	extern uint64_t arch_vm_get_paddr(uint64_t *root, uint64_t vaddr);
	uint64_t start = addr & ~(uint64_t)(PAGE_SIZE - 1);
	uint64_t end   = (addr + len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	for (uint64_t va = start; va < end; va += PAGE_SIZE) {
		vma_t *v = anon_vma_at(p, va);
		if (!v || (v->flags & VMA_SHARED)) continue;
		if (!arch_vm_get_paddr(p->vm->root, va)) continue;
		vm_unmap(p->vm, va);
		cgroup_uncharge_mem(p->cgroup, PAGE_SIZE);
		p->mem_charged -= PAGE_SIZE;
	}
}

static long sys_madvise(uint64_t addr, size_t len, int advice)
{
	process_t *p = task_current();
	switch (advice) {
	case MADV_HUGEPAGE: {
		uint64_t start = (addr + HUGE_PAGE_SIZE - 1) & HUGE_PAGE_MASK;
		uint64_t end   = (addr + len) & HUGE_PAGE_MASK;
		for (uint64_t va = start; va < end; va += HUGE_PAGE_SIZE)
			thp_collapse(p, va);
		return 0;
	}
	case MADV_WILLNEED:        mm_prefault(p, addr, len); return 0;
	case MADV_DONTNEED:
	case MADV_FREE:            mm_drop(p, addr, len);     return 0;
	default:                   return 0;   /* NORMAL/RANDOM/SEQUENTIAL/NOHUGEPAGE: advisory */
	}
}

/* Mark/clear VMA_LOCKED across [addr, addr+len), splitting any VMA the range only
 * partly covers so the flag lands on exactly the requested span (same carve as
 * mprotect). There is no reclaim yet, so the flag's effect today is the prefault
 * mlock already did plus the promise that future reclaim skips it. */
static void mm_set_locked(process_t *p, uint64_t addr, size_t len, int on)
{
	uint64_t end = addr + len;
	for (vma_t *v = p->vmas; v; v = v->next) {
		uint64_t vstart = v->vaddr, vend = v->vaddr + v->size;
		if (vend <= addr || vstart >= end) continue;
		uint64_t lo = addr > vstart ? addr : vstart;
		uint64_t hi = end < vend ? end : vend;
		uint32_t newf = on ? (v->flags | VMA_LOCKED) : (v->flags & ~(uint32_t)VMA_LOCKED);
		if (newf == v->flags) continue;
		if (vstart == lo && vend == hi) {
			v->flags = newf;
		} else {
			uint32_t oldf = v->flags;
			if (vstart < lo) vma_add(p, vstart, lo - vstart, oldf);
			if (hi < vend)   vma_add(p, hi, vend - hi, oldf);
			v->vaddr = lo; v->size = hi - lo; v->flags = newf;
		}
	}
}

static long sys_mlock(uint64_t addr, size_t len)
{
	if (len == 0) return 0;
	process_t *p = task_current();
	uint64_t start = addr & ~(uint64_t)(PAGE_SIZE - 1);
	len = (addr - start) + len;
	if (!access_ok((void *)start, len)) return -12;   /* ENOMEM: range outside user space */
	mm_prefault(p, start, len);
	mm_set_locked(p, start, len, 1);
	return 0;
}

static long sys_munlock(uint64_t addr, size_t len)
{
	if (len == 0) return 0;
	process_t *p = task_current();
	uint64_t start = addr & ~(uint64_t)(PAGE_SIZE - 1);
	len = (addr - start) + len;
	if (!access_ok((void *)start, len)) return -12;
	mm_set_locked(p, start, len, 0);
	return 0;
}

#define MLOCK_ONFAULT 0x01

static long sys_mlock2(uint64_t addr, size_t len, int flags)
{
	if (flags & ~MLOCK_ONFAULT) return -22;
	if (flags & MLOCK_ONFAULT) {   /* lock present pages now, lock the rest as they fault */
		if (len == 0) return 0;
		process_t *p = task_current();
		uint64_t start = addr & ~(uint64_t)(PAGE_SIZE - 1);
		len = (addr - start) + len;
		if (!access_ok((void *)start, len)) return -12;
		mm_set_locked(p, start, len, 1);
		return 0;
	}
	return sys_mlock(addr, len);
}

#define MCL_CURRENT 1
#define MCL_FUTURE  2
#define MCL_ONFAULT 4

/* Lock all current mappings (MCL_CURRENT) and/or arrange for future ones to be
 * locked as they are created (MCL_FUTURE). mmap already populates anonymous
 * regions, so the future effect is the VMA_LOCKED flag carried forward. */
static long sys_mlockall(int flags)
{
	if (!(flags & (MCL_CURRENT | MCL_FUTURE)) || (flags & ~(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT)))
		return -22;
	process_t *p = task_current();
	p->mlock_future = (flags & MCL_FUTURE) ? 1 : 0;
	if (flags & MCL_CURRENT) {
		for (vma_t *v = p->vmas; v; v = v->next) {
			if (!(flags & MCL_ONFAULT)) mm_prefault(p, v->vaddr, v->size);
			v->flags |= VMA_LOCKED;
		}
	}
	return 0;
}

static long sys_munlockall(void)
{
	process_t *p = task_current();
	p->mlock_future = 0;
	for (vma_t *v = p->vmas; v; v = v->next)
		v->flags &= ~(uint32_t)VMA_LOCKED;
	return 0;
}

static long sys_msync(uint64_t addr, size_t len, int flags)
{
	(void)addr; (void)len; (void)flags;
	return 0;
}
