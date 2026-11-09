/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
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
		if (oom_kill_alloc_ex(1) != OOM_KILLED_OTHER) break;
		pg = buddy_alloc(0);
	}
	return pg;
}

/* Grow or shrink the heap. addr==0 queries the current break. */
static uint64_t brk_set(process_t *p, uint64_t addr)
{
	vm_space_t *vm = p->vm;
	uint64_t cur = vm->brk;
	if (addr == 0 || !access_ok((void *)addr, 1)) return cur;   /* rejects kernel-range brk */

	uint64_t new_brk = (addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

	/* Pages are mapped eagerly here: a request past all of RAM would only run the
	 * OOM killer over other tasks, and one reaching a mapping would replace it. */
	if (new_brk > cur) {
		if ((new_brk - cur) / PAGE_SIZE > buddy_total_pages()) return cur;
		uint64_t cf = spin_lock_irq(&vm->fault_lock);
		int hit = 0;
		for (vma_t *v = vm->vmas; v && !hit; v = v->next)
			hit = v->vaddr < new_brk && v->vaddr + v->size > cur;
		spin_unlock_irq(&vm->fault_lock, cf);
		if (hit) return cur;
	}

	if (new_brk < cur) {
		/* Shrink: free pages above new_brk */
		for (uint64_t va = new_brk; va < cur; va += PAGE_SIZE)
			vm_unmap(vm, va);
		vm->brk = new_brk;
		return new_brk;
	}

	for (uint64_t va = cur; va < new_brk; va += PAGE_SIZE) {
		void *page = NULL;
		if (cgroup_charge_mem(vm, p->cgroup, PAGE_SIZE) == 0) {
			page = mm_alloc_page_oom();
			if (!page) cgroup_uncharge_mem(vm, PAGE_SIZE);
		}
		if (!page) {
			for (uint64_t u = cur; u < va; u += PAGE_SIZE)
				vm_unmap(vm, u);
			return cur;
		}
		memset((void *)PHYS_TO_VIRT(page), 0, PAGE_SIZE);
		vm_map(vm, va, (uint64_t)page, VM_USER | VM_READ | VM_WRITE);
	}
	vm->brk = new_brk;
	return new_brk;
}

static long sys_brk(uint64_t addr)
{
	process_t *p = task_current();
	mutex_lock(&p->vm->brk_lock);
	uint64_t r = brk_set(p, addr);
	mutex_unlock(&p->vm->brk_lock);
	return (long)r;
}

/* Map an anonymous or file-backed region. Anonymous pages are zero-filled on
 * demand here; file-backed pages are read in a page at a time. The resulting
 * region is recorded as a VMA carrying the requested protection. */
#define MAP_POPULATE_ 0x8000

/* Remove whatever a MAP_FIXED mapping lands on. Past a gigabyte the range is
 * walked through the VMAs, since the per-page walk would take seconds. */
__attribute__((weak)) uint64_t vm_skip_empty(vm_space_t *vs, uint64_t vaddr) { (void)vs; return vaddr; }

static void unmap_span(process_t *p, uint64_t lo, uint64_t hi)
{
	for (uint64_t v = lo; v < hi; ) {
		uint64_t n = vm_skip_empty(p->vm, v);
		if (n > v) { v = n; continue; }      /* no table here: nothing mapped */
		vm_unmap(p->vm, v);
		v += PAGE_SIZE;
	}
}

static void drop_old_pages(process_t *p, uint64_t va, uint64_t len)
{
	filecopy_drop(p->vm, va, len);   /* a shared copy of a file writes back first */
	if (len <= (1UL << 30)) {
		unmap_span(p, va, va + len);
		return;
	}
	for (vma_t *m = p->vm->vmas; m; m = m->next) {
		uint64_t lo = m->vaddr > va ? m->vaddr : va;
		uint64_t me = m->vaddr + m->size, re = va + len;
		uint64_t hi = me < re ? me : re;
		if (lo < hi) unmap_span(p, lo, hi);
	}
}

/* Back out a mapping of len bytes charged up front and built as far as v.
 * Unmapping uncharges what was mapped; the rest never got a page. */
static void map_undo(process_t *p, uint64_t va, uint64_t v, uint64_t len)
{
	for (uint64_t u = va; u < v; u += PAGE_SIZE)
		vm_unmap(p->vm, u);
	cgroup_uncharge_mem(p->vm, (long)(len - (v - va)));
}

#ifdef ARCH_USER_VA_END
#define USER_SPAN ARCH_USER_VA_END
#else
#define USER_SPAN (1UL << 39)
#endif

/* PROT_* as the VMA's permission bits. */
static uint32_t vma_prot(int prot)
{
	uint32_t vf = 0;
	if (prot & PROT_READ)  vf |= VMA_READ;
	if (prot & PROT_WRITE) vf |= VMA_WRITE;
	if (prot & PROT_EXEC)  vf |= VMA_EXEC;
	return vf;
}

/* bytes the space maps (its VMAs), for RLIMIT_AS and mlockall */
static uint64_t mapped_bytes(process_t *p)
{
	uint64_t used = 0;
	uint64_t cf = spin_lock_irq(&p->vm->fault_lock);
	for (vma_t *v = p->vm->vmas; v; v = v->next) used += v->size;
	spin_unlock_irq(&p->vm->fault_lock, cf);
	return used;
}

/* RLIMIT_AS: would a mapping of len bytes push the address space over the
 * soft limit? (rlim_set==0 means uninitialized → unlimited.) */
static int mmap_over_rlimit(process_t *p, size_t len)
{
	if (p->rlim_set && p->rlim_cur[RLIMIT_AS] != RLIM_INFINITY) {
		uint64_t req = (len + 0xFFF) & ~0xFFFULL;
		if (mapped_bytes(p) + req > p->rlim_cur[RLIMIT_AS]) return 1;
	}
	return 0;
}

/* RLIMIT_MEMLOCK (Linux can_do_mlock, mlock_future_ok): locked memory cannot be
 * reclaimed, so a task without CAP_IPC_LOCK may hold at most the limit locked,
 * and nothing at all when the limit is 0. Locked memory is counted as the
 * space's VMA_LOCKED bytes (Linux mm->locked_vm). Until a limit is set
 * (rlim_set) it is unlimited, as before. */
static uint64_t memlock_limit(process_t *p)
{
	if (!p->rlim_set || capable_task((struct process *)p, CAP_IPC_LOCK)) return RLIM_INFINITY;
	return p->rlim_cur[RLIMIT_MEMLOCK];
}

/* locked bytes in the space; *inside: how many of them lie in [start, end) */
static uint64_t locked_bytes(process_t *p, uint64_t start, uint64_t end, uint64_t *inside)
{
	uint64_t total = 0, in = 0;
	uint64_t cf = spin_lock_irq(&p->vm->fault_lock);
	for (vma_t *v = p->vm->vmas; v; v = v->next) {
		if (!(v->flags & VMA_LOCKED)) continue;
		total += v->size;
		uint64_t lo = v->vaddr > start ? v->vaddr : start;
		uint64_t hi = v->vaddr + v->size < end ? v->vaddr + v->size : end;
		if (hi > lo) in += hi - lo;
	}
	spin_unlock_irq(&p->vm->fault_lock, cf);
	if (inside) *inside = in;
	return total;
}

/* Is every page of [start, end) in some VMA? mlock and munlock of a hole are ENOMEM. */
static int range_mapped(process_t *p, uint64_t start, uint64_t end)
{
	uint64_t cur = start;
	uint64_t cf = spin_lock_irq(&p->vm->fault_lock);
	int moved = 1;
	while (cur < end && moved) {
		moved = 0;
		for (vma_t *v = p->vm->vmas; v; v = v->next)
			if (cur >= v->vaddr && cur < v->vaddr + v->size) { cur = v->vaddr + v->size; moved = 1; }
	}
	spin_unlock_irq(&p->vm->fault_lock, cf);
	return cur >= end;
}

/* A new mapping that will be locked (MAP_LOCKED, or after mlockall(MCL_FUTURE)):
 * EPERM for MAP_LOCKED at limit 0, EAGAIN past the limit, as Linux do_mmap. */
static long mmap_memlock_check(process_t *p, size_t len, int flags)
{
	if (!(flags & MAP_LOCKED) && !p->mlock_future) return 0;
	uint64_t lim = memlock_limit(p);
	if (lim == RLIM_INFINITY) return 0;
	if ((flags & MAP_LOCKED) && lim == 0) return -EPERM;
	uint64_t req = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	return locked_bytes(p, 0, 0, NULL) + req > lim ? -EAGAIN : 0;
}

/* MAP_FIXED_NOREPLACE: MAP_FIXED that fails with EEXIST over anything already
 * there. Wine lays out its address space with it. 0: the range is free. */
static long mmap_noreplace_check(process_t *p, uint64_t addr, size_t len)
{
	if (addr & (PAGE_SIZE - 1)) return -EINVAL;
	if (!access_ok((void *)addr, len)) return -ENOMEM;
	uint64_t end = addr + ((len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1));
	int hit = arch_vm_get_paddr(p->vm->root, addr) != 0;   /* the heap has no VMA */
	uint64_t cf = spin_lock_irq(&p->vm->fault_lock);
	for (vma_t *v = p->vm->vmas; v && !hit; v = v->next)
		hit = v->vaddr < end && v->vaddr + v->size > addr;
	spin_unlock_irq(&p->vm->fault_lock, cf);
	if (hit) return -EEXIST;
	return 0;
}

/* MAP_HUGETLB | MAP_HUGE_1GB: back the region with 1 GB blocks from the
 * reserved pool, in the process's own high VA window. Eagerly populated. */
static long mmap_huge_1g(process_t *p, uint64_t addr, size_t len, int prot, int flags, uint32_t vmflags)
{
	if (len > (uint64_t)-1 - (HUGE_1G_SIZE - 1)) return -ENOMEM;   /* rounding would wrap */
	len = (len + HUGE_1G_SIZE - 1) & HUGE_1G_MASK;
	uint64_t va;
	if ((flags & MAP_FIXED) && addr) va = addr & HUGE_1G_MASK;
	else {
		uint64_t cf = spin_lock_irq(&p->vm->fault_lock);
		uint64_t next = (p->vm->huge1g_next + HUGE_1G_SIZE - 1) & HUGE_1G_MASK;
		if (next + len < next) { spin_unlock_irq(&p->vm->fault_lock, cf); return -ENOMEM; }   /* cursor overflow */
		va = next; p->vm->huge1g_next = next + len;
		spin_unlock_irq(&p->vm->fault_lock, cf);
	}
	if ((flags & MAP_FIXED) && addr) drop_old_pages(p, va, len);
	if (cgroup_charge_mem(p->vm, p->cgroup, (long)len) != 0) { cgroup_oom_kill(p->cgroup); return -ENOMEM; }
	for (uint64_t v = va; v < va + len; v += HUGE_1G_SIZE) {
		uint64_t blk = huge1g_alloc();
		if (!blk || vm_map_huge_1g(p->vm, v, blk, vmflags) != 0) {
			if (blk) huge1g_put(blk);
			for (uint64_t u = va; u < v; u += HUGE_1G_SIZE) vm_unmap(p->vm, u);
			cgroup_uncharge_mem(p->vm, (long)(len - (v - va)));
			return -ENOMEM;
		}
		memset((void *)PHYS_TO_VIRT(blk), 0, HUGE_1G_SIZE);
	}
	uint32_t vf = VMA_USER | VMA_ANON | VMA_HUGE | VMA_HUGE_1G | vma_prot(prot);
	if (flags & MAP_SHARED) vf |= VMA_SHARED;
	vma_replace(p, va, len, vf);
	return (long)va;
}

/* MAP_HUGETLB: back the region with 2 MB blocks. Both length and address
 * round to the huge-page boundary; pages are populated eagerly so there is
 * no demand-fault path to teach about blocks. */
static long mmap_huge_2m(process_t *p, uint64_t addr, size_t len, int prot, int flags, uint32_t vmflags)
{
	if (len > (uint64_t)-1 - (HUGE_PAGE_SIZE - 1)) return -ENOMEM;   /* rounding would wrap */
	len = (len + HUGE_PAGE_SIZE - 1) & HUGE_PAGE_MASK;
	uint64_t va;
	if ((flags & MAP_FIXED) && addr) va = addr & HUGE_PAGE_MASK;
	else {
		uint64_t ceil = HUGE1G_VA_BASE;   /* mmap window ends below the 1 GB-hugepage window, itself below the stack */
		uint64_t cf = spin_lock_irq(&p->vm->fault_lock);
		uint64_t next = (p->vm->mmap_next + HUGE_PAGE_SIZE - 1) & HUGE_PAGE_MASK;
		if (next + len > ceil || next + len < next) { spin_unlock_irq(&p->vm->fault_lock, cf); return -ENOMEM; }   /* over window / wrap */
		va = next; p->vm->mmap_next = next + len;
		spin_unlock_irq(&p->vm->fault_lock, cf);
	}
	if ((flags & MAP_FIXED) && addr) drop_old_pages(p, va, len);
	if (cgroup_charge_mem(p->vm, p->cgroup, (long)len) != 0) { cgroup_oom_kill(p->cgroup); return -ENOMEM; }
	for (uint64_t v = va; v < va + len; v += HUGE_PAGE_SIZE) {
		void *blk = buddy_alloc(9);   /* 2 MB = 512 pages */
		if (!blk || vm_map_huge(p->vm, v, (uint64_t)blk, vmflags) != 0) {
			if (blk) buddy_free((uint64_t)blk, 9);
			for (uint64_t u = va; u < v; u += HUGE_PAGE_SIZE) vm_unmap(p->vm, u);
			cgroup_uncharge_mem(p->vm, (long)(len - (v - va)));
			return -ENOMEM;
		}
		memset((void *)PHYS_TO_VIRT(blk), 0, HUGE_PAGE_SIZE);
	}
	uint32_t vf = VMA_USER | VMA_ANON | VMA_HUGE | vma_prot(prot);
	if (flags & MAP_SHARED) vf |= VMA_SHARED;
	vma_replace(p, va, len, vf);
	return (long)va;
}

/* Pick the address of a page-granular mapping: the caller's for MAP_FIXED,
 * else the next free stretch of the mmap window. 0, or -ENOMEM: no room. */
static long mmap_pick_va(process_t *p, uint64_t addr, size_t len, int flags, uint64_t *vap)
{
	if ((flags & MAP_FIXED) && addr) { *vap = addr & ~(uint64_t)(PAGE_SIZE - 1); return 0; }
	/* The bump cursor lives on the (thread-shared) address space; bump it
	 * under fault_lock so two threads mmap'ing at once get distinct ranges
	 * instead of the same VA. The window ends below the 1 GB-hugepage window
	 * (itself below the stack); refuse a VA that would overrun it (Linux
	 * returns ENOMEM when no region fits) rather than colliding. */
	uint64_t ceil = HUGE1G_VA_BASE;
	uint64_t cf = spin_lock_irq(&p->vm->fault_lock);
	if (p->vm->mmap_next + len > ceil || p->vm->mmap_next + len < p->vm->mmap_next) {
		spin_unlock_irq(&p->vm->fault_lock, cf);
		return -ENOMEM;
	}
	*vap = p->vm->mmap_next; p->vm->mmap_next += len;
	spin_unlock_irq(&p->vm->fault_lock, cf);
	return 0;
}

/* Anonymous memory at va. */
static long mmap_anon(process_t *p, uint64_t va, size_t len, int prot, int flags, uint32_t vmflags)
{
	/* Private anonymous memory is demand-zero: vm_handle_fault supplies each
	 * page from the VMA on first touch. Runtimes reserve far more address
	 * space than they use (JavaScriptCore asks for 128 GB), and only a fresh
	 * VA from the bump cursor is known to hold no old pages. */
	int lazy = !(flags & MAP_SHARED) && (prot & PROT_READ) &&
	           !(flags & (MAP_LOCKED | MAP_POPULATE_)) && !p->mlock_future;
	if (prot != 0 && !lazy) {
		/* eager: past all of RAM this only OOM-kills other tasks before failing */
		if (len / PAGE_SIZE > buddy_total_pages()) return -ENOMEM;
		if (cgroup_charge_mem(p->vm, p->cgroup, (long)len) != 0) { cgroup_oom_kill(p->cgroup); return -ENOMEM; } /* over memory.max */
		for (uint64_t v = va; v < va + len; v += PAGE_SIZE) {
			void *pg = mm_alloc_page_oom();
			if (!pg) { map_undo(p, va, v, len); kprintf(KERN_WARN "mm: mmap out of memory pid=%u va=0x%lx len=%zu free=%lu\n", p->pid, va, len, pmm_free_pages()); return -ENOMEM; }
			memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
			vm_map(p->vm, v, (uint64_t)pg, vmflags);
		}
	}
	uint32_t vf = VMA_USER | VMA_ANON | vma_prot(prot);
	if (flags & MAP_SHARED) vf |= VMA_SHARED;
	if ((flags & MAP_LOCKED) || p->mlock_future) vf |= VMA_LOCKED;
	vma_replace(p, va, len, vf);
	return (long)va;
}

/* A file-backed mapping as sys_mmap checked it. f stays pinned by the caller. */
typedef struct {
	process_t  *p;
	vfs_file_t *f;
	uint64_t    va, off;
	size_t      len;
	int         prot, flags, ro_shared;
	int         noexec;     /* the file's mount is noexec: never PROT_EXEC */
	uint32_t    vmflags;
} mmap_file_t;

/* Freshly mapped instructions must reach the instruction cache (aarch64 keeps
 * the two caches apart). */
static void mmap_sync_icache(uintptr_t pp)
{
#if defined(__aarch64__)
	for (uintptr_t l = pp; l < pp + PAGE_SIZE; l += 64)
		__asm__ volatile("dc cvac, %0" :: "r"(l) : "memory");
	__asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");
#else
	(void)pp;
#endif
}

/* The file hands out its own pages one at a time (mmap_page fop). */
static long mmap_file_pages(const mmap_file_t *m)
{
	process_t *p = m->p; vfs_file_t *f = m->f;
	uint64_t va = m->va; size_t len = m->len; int prot = m->prot;
	int shared = (m->flags & MAP_SHARED) != 0;
	int swrite = shared && (prot & PROT_WRITE);
	uint32_t pte_flags = shared ? m->vmflags : m->vmflags & ~(uint32_t)VM_WRITE;
	uint64_t file_off = m->off;
	for (uint64_t v = va; v < va + len; v += PAGE_SIZE) {
		uint64_t phys = f->inode->fops->mmap_page(f, (off_t)file_off, swrite);
		if (!phys) {
			for (uint64_t u = va; u < v; u += PAGE_SIZE) vm_unmap(p->vm, u);
			return -ENOMEM;
		}
		vm_map(p->vm, v, phys, pte_flags);
		file_off += PAGE_SIZE;
		if (prot & PROT_EXEC) mmap_sync_icache((uintptr_t)PHYS_TO_VIRT(phys));
	}
	uint32_t vf = VMA_USER | vma_prot(prot);
	if (shared) vf |= VMA_SHARED;
	if (m->ro_shared) vf |= VMA_NOWRITE;
	if (m->noexec) vf |= VMA_NOEXEC;
	vma_replace(p, va, len, vf);
	return (long)va;
}

/* Mapping backed by an inode-owned page via the mmap fop. Two kinds:
 *  - cacheable (ramfs/tmpfs): the fop returns a page of the file's own
 *    write-back buffer. MAP_SHARED maps it directly so stores land in the
 *    file and are seen by read() and other mappers. The fop's extra page ref
 *    is the mapping's: unmap and exit drop it, so the page outlives the file
 *    while mapped.
 *    MAP_PRIVATE takes the copy path (mmap_file_copy) for COW semantics.
 *  - device/hugetlb: non-cacheable, no write-back. */
static long mmap_file_fop(const mmap_file_t *m)
{
	process_t *p = m->p; vfs_file_t *f = m->f;
	uint64_t va = m->va; size_t len = m->len;
	int cacheable = f->inode->fops->mmap_cacheable;
	uint64_t file_off = m->off;
	uint32_t pte_flags = m->vmflags | (cacheable ? 0 : VM_NOCACHE);
#ifdef __x86_64__
	if (f->inode->fops->mmap_wc) pte_flags = (pte_flags & ~(uint32_t)VM_NOCACHE) | VM_WC;
#endif
	if (f->inode->fops->mmap_keep_pages) pte_flags |= VM_NOFREE;
	for (uint64_t v = va; v < va + len; v += PAGE_SIZE) {
		uint64_t phys = f->inode->fops->mmap(f, (off_t)file_off);
		if (!phys) {
			for (uint64_t u = va; u < v; u += PAGE_SIZE) vm_unmap(p->vm, u);
			return -EINVAL;   /* offset past backing */
		}
		vm_map(p->vm, v, phys, pte_flags);
		file_off += PAGE_SIZE;
	}
	uint32_t vf = VMA_USER | vma_prot(m->prot);
	if ((cacheable || f->inode->fops->mmap_keep_pages) && (m->flags & MAP_SHARED))
		vf |= VMA_SHARED;
	if (m->ro_shared) vf |= VMA_NOWRITE;
	if (m->noexec) vf |= VMA_NOEXEC;
	vma_replace(p, va, len, vf);
	return (long)va;
}

/* Copy: read the file into fresh pages, eagerly. MAP_SHARED pages are shared
 * across fork, and what changes in them goes back to the file (filecopy.h). */
static long mmap_file_copy(const mmap_file_t *m)
{
	process_t *p = m->p; vfs_file_t *f = m->f;
	uint64_t va = m->va; size_t len = m->len;
	if (!f->inode->fops || !f->inode->fops->read) return -ENODEV;
	if (len / PAGE_SIZE > buddy_total_pages()) return -ENOMEM;      /* copied eagerly */
	if (cgroup_charge_mem(p->vm, p->cgroup, (long)len) != 0) return -ENOMEM;

	uint64_t file_off = m->off;
	for (uint64_t v = va; v < va + len; v += PAGE_SIZE) {
		void *pg = buddy_alloc(0);
		if (!pg) { map_undo(p, va, v, len); return -ENOMEM; }
		void *pg_virt = (void *)PHYS_TO_VIRT(pg);
		memset(pg_virt, 0, PAGE_SIZE);

		/* read one page from file at current offset via a temporary file handle */
		vfs_file_t tmp_f = *f;
		tmp_f.pos = (off_t)file_off;
		ssize_t nr = f->inode->fops->read(&tmp_f, pg_virt, PAGE_SIZE);
		(void)nr;
		file_off += PAGE_SIZE;

		vm_map(p->vm, v, (uint64_t)pg, m->vmflags);
		if (m->prot & PROT_EXEC) mmap_sync_icache((uintptr_t)pg_virt);
	}
	uint32_t vf = VMA_USER | vma_prot(m->prot);
	if (m->flags & MAP_SHARED) vf |= VMA_SHARED;
	if (m->ro_shared) vf |= VMA_NOWRITE;
	if (m->noexec) vf |= VMA_NOEXEC;
	vma_replace(p, va, len, vf);
	if ((m->flags & MAP_SHARED) && !m->ro_shared && S_ISREG(f->inode->mode) && f->inode->fops->write &&
	    filecopy_add(p->vm, va, len, f, m->off) != 0) {
		drop_old_pages(p, va, len);
		vma_clip(p, va, len);
		return -ENOMEM;
	}
	return (long)va;
}

/* File-backed mmap at va: checks the open mode, then maps by the file's kind. */
static long mmap_file(process_t *p, uint64_t va, size_t len, int prot, int flags, int fd, uint64_t off, uint32_t vmflags)
{
	vfs_file_t *f = fget(p->fds, fd);   /* pin across the blocking map loop */
	if (!f) return -EBADF;
	long r;

	/* Linux do_mmap: EPERM here, and EACCES from a later mprotect (VMA_NOEXEC) */
	if ((prot & PROT_EXEC) && (f->mnt_flags & MS_NOEXEC)) { r = -EPERM; goto out; }
	/* a mapping reads the file; O_WRONLY may have been all the opener could get */
	if (f->path[0] == '/' && (f->flags & 3) == O_WRONLY) { r = -EACCES; goto out; }

	/* stores through a shared mapping land in the file; files with no path, like
	 * the io_uring rings, carry no access mode */
	int ro_shared = (flags & MAP_SHARED) && f->path[0] == '/' && (f->flags & 3) == O_RDONLY;
	if (ro_shared && (prot & PROT_WRITE)) { r = -EACCES; goto out; }

	mmap_file_t m = { .p = p, .f = f, .va = va, .off = off, .len = len, .prot = prot,
	                  .flags = flags, .ro_shared = ro_shared, .vmflags = vmflags,
	                  .noexec = (f->mnt_flags & MS_NOEXEC) != 0 };
	const vfs_fops_t *ops = f->inode->fops;
	if (ops && ops->mmap_page)
		r = mmap_file_pages(&m);
	else if (ops && ops->mmap && (!ops->mmap_cacheable || (flags & MAP_SHARED)))
		r = mmap_file_fop(&m);
	else
		r = mmap_file_copy(&m);
out:
	fput(f);
	return r;
}

static long sys_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, uint64_t off)
{
	if (len == 0 || (off & (PAGE_SIZE - 1)) || (int64_t)off < 0 || off + len < off) return -EINVAL;
	if (!(flags & (MAP_SHARED | MAP_PRIVATE))) return -EINVAL;
	if ((flags & MAP_FIXED) && (addr & (PAGE_SIZE - 1))) return -EINVAL;
	if (len >= USER_SPAN) return -ENOMEM;   /* more than all of user space; rounding must not wrap */
	process_t *p = task_current();
	if ((flags & (MAP_FIXED | 0x100000 /* MAP_FIXED_NOREPLACE */)) && addr < MMAP_MIN_ADDR &&
	    !capable_task(p, CAP_SYS_RAWIO))
		return -EPERM;

	if (mmap_over_rlimit(p, len)) return -ENOMEM;
	long ml = mmap_memlock_check(p, len, flags);
	if (ml) return ml;

	if ((flags & 0x100000) && addr) {   /* MAP_FIXED_NOREPLACE */
		long e = mmap_noreplace_check(p, addr, len);
		if (e) return e;
		flags |= MAP_FIXED;
	}

	/* MAP_FIXED maps at a user-chosen address; reject a kernel-range target so it
	 * cannot write a PTE over the (shared) kernel half of the page tables. */
	if ((flags & MAP_FIXED) && addr && !access_ok((void *)addr, len)) return -EINVAL;

	/* JITs (V8, the JVM, LuaJIT) need W+X by default, as on Linux. A process
	 * that wants W^X opts in with prctl(PR_SET_MDWE) and keeps it across exec. */
	if (task_current()->mdwe && (prot & PROT_WRITE) && (prot & PROT_EXEC))
		return -EACCES;

	uint32_t vmflags = VM_USER;
	if (prot & PROT_READ)  vmflags |= VM_READ;
	if (prot & PROT_WRITE) vmflags |= VM_WRITE;
	if (prot & PROT_EXEC)  vmflags |= VM_EXEC;

	if ((flags & MAP_HUGETLB) && ((flags >> MAP_HUGE_SHIFT) & 0x3F) == 30 &&
	    (flags & MAP_ANONYMOUS) && prot != 0)
		return mmap_huge_1g(p, addr, len, prot, flags, vmflags);
	if ((flags & MAP_HUGETLB) && (flags & MAP_ANONYMOUS) && prot != 0)
		return mmap_huge_2m(p, addr, len, prot, flags, vmflags);

	len = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	uint64_t va;
	if (mmap_pick_va(p, addr, len, flags, &va) != 0) return -ENOMEM;
	/* a page left under the new one would be lost to its owner and to the allocator */
	if (flags & MAP_FIXED) drop_old_pages(p, va, len);

	if (flags & MAP_ANONYMOUS)
		return mmap_anon(p, va, len, prot, flags, vmflags);
	return mmap_file(p, va, len, prot, flags, fd, off, vmflags);
}

/* Unmap a range and drop any VMA fully contained in it. */
static long sys_munmap(uint64_t addr, size_t len)
{
	process_t *p  = task_current();
	if (!len || (addr & (PAGE_SIZE - 1))) return -EINVAL;
	if (len >= USER_SPAN) return -EINVAL;
	len           = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	if (!access_ok((void *)addr, len)) return -EINVAL;   /* user range only */
	drop_old_pages(p, addr, len);                    /* a per-page walk of 512 GB cannot be killed */
	/* Drop the VMA coverage for exactly [addr, addr+len): trims VMAs the range
	 * partially overlaps and splits one it punches a hole in, not just the fully
	 * covered ones. A stale VMA left over a now-unmapped slice would otherwise
	 * win the fault lookup with the old region's permissions — glibc's malloc and
	 * ld.so unmap sub-ranges of larger reservations constantly. */
	vma_clip(p, addr, len);
	return 0;
}

/* Change a range's protection. The PTEs are remapped and the covering VMAs'
 * flags are updated so the fault handler enforces the new protection (a write
 * to a now read-only page must fault). */
static long sys_mprotect(uint64_t addr, size_t len, int prot)
{
	if (addr & (PAGE_SIZE - 1)) return -EINVAL;
	if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC | 0x03000000 /* GROWSDOWN/UP */)) return -EINVAL;
	if (len == 0) return 0;
	process_t *p = task_current();
	if (len >= USER_SPAN) return -ENOMEM;
	len  = (len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
	if (!access_ok((void *)addr, len)) return -EINVAL;   /* user range only */

	/* JITs (V8, the JVM, LuaJIT) need W+X by default, as on Linux. A process
	 * that wants W^X opts in with prctl(PR_SET_MDWE) and keeps it across exec. */
	if (task_current()->mdwe && (prot & PROT_WRITE) && (prot & PROT_EXEC))
		return -EACCES;

	uint32_t vmflags = VM_USER;
	if (prot & PROT_READ)  vmflags |= VM_READ;
	if (prot & PROT_WRITE) vmflags |= VM_WRITE;
	if (prot & PROT_EXEC)  vmflags |= VM_EXEC;

	/* Update VMA permissions BEFORE touching the PTEs, under the space's
	 * fault_lock. With the VMA list shared across threads, a sibling faulting
	 * mid-mprotect must observe the new protection — if it saw the old (still
	 * readable) flags after the page was unmapped, the fault handler would
	 * demand-fault a fresh zero page into a region being made PROT_NONE. Doing
	 * the flag update first means a racing fault either reads the still-mapped
	 * page (benign, about to change) or sees the new prot and is denied. Split
	 * any VMA the range only partially covers so the new prot applies to exactly
	 * [addr, addr+len) — applying it to the whole VMA would e.g. strip write from
	 * a binary's data segment when ld.so makes its small RELRO sub-region RO. */
	uint32_t vmaflags = VMA_USER;
	if (prot & PROT_READ)  vmaflags |= VMA_READ;
	if (prot & PROT_WRITE) vmaflags |= VMA_WRITE;
	if (prot & PROT_EXEC)  vmaflags |= VMA_EXEC;
	uint64_t vf = spin_lock_irq(&p->vm->fault_lock);
	/* what the mapping was refused at mmap time it cannot gain here */
	uint32_t never = ((prot & PROT_WRITE) ? VMA_NOWRITE : 0) | ((prot & PROT_EXEC) ? VMA_NOEXEC : 0);
	if (never)
		for (vma_t *v = p->vm->vmas; v; v = v->next)
			if ((v->flags & never) && v->vaddr < addr + len && v->vaddr + v->size > addr) {
				spin_unlock_irq(&p->vm->fault_lock, vf);
				return -EACCES;
			}
	for (vma_t *v = p->vm->vmas; v; v = v->next) {
		uint64_t vstart = v->vaddr, vend = v->vaddr + v->size;
		if (vend <= addr || vstart >= addr + len) continue; /* no overlap */
		uint64_t lo = addr > vstart ? addr : vstart;
		uint64_t hi = (addr + len) < vend ? (addr + len) : vend;
		uint32_t newf = vmaflags | (v->flags & (VMA_SHARED | VMA_ANON | VMA_NOWRITE | VMA_NOEXEC));
		if (vstart == lo && vend == hi) {
			v->flags = newf;                       /* whole VMA */
		} else {
			uint32_t oldf = v->flags;              /* carve out the middle */
			if (vstart < lo) vma_add_locked(p->vm, vstart, lo - vstart, oldf);
			if (hi < vend)   vma_add_locked(p->vm, hi, vend - hi, oldf);
			v->vaddr = lo; v->size = hi - lo; v->flags = newf;
		}
	}
	spin_unlock_irq(&p->vm->fault_lock, vf);

	/* PTEs are updated after the VMA flags are already published, and each
	 * vm_map/vm_unmap takes fault_lock itself (so this loop must not hold it).
	 * A sibling faulting in the gap sees the new VMA prot: if the page is still
	 * mapped a read just succeeds, and once it is unmapped the PROT_NONE VMA
	 * denies the refault — never a fresh zero page into a protected region. */
	for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE) {
		uint64_t pa = arch_vm_get_paddr(p->vm->root, va);
		if (pa) {
			if (prot == 0) {
				vm_unmap(p->vm, va);
			} else {
				/* A page others hold (fork, the file cache) must stay read-only
				 * in a private mapping: the write fault makes the copy. */
				uint32_t fl = vmflags;
				page_t *pg = (fl & VM_WRITE) ? paddr_to_page(pa) : NULL;
				if (pg && pg->refcount > 1) {
					vma_t *m = p->vm->vmas;
					while (m && !(va >= m->vaddr && va < m->vaddr + m->size)) m = m->next;
					if (m && !(m->flags & VMA_SHARED)) fl &= ~(uint32_t)VM_WRITE;
				}
				vm_map(p->vm, va, pa, fl);
			}
		} else {
			/* An untouched anonymous page faults in with the VMA's new
			 * protection, so skip the rest of that VMA instead of filling it. */
			vma_t *m = p->vm->vmas;
			while (m && !(va >= m->vaddr && va < m->vaddr + m->size)) m = m->next;
			/* nothing there: refuse, or this fills all of an address range with pages */
			if (!m) { arch_tlb_flush_range(p->vm, addr, addr + len); return -ENOMEM; }
			if (prot == 0) continue;
			if (m && (m->flags & VMA_ANON) && !(m->flags & VMA_SHARED) && (m->flags & VMA_READ)) {
				uint64_t end = m->vaddr + m->size;
				for (uint64_t x = va; x < end && x < addr + len; x += PAGE_SIZE) {
					if ((end - x) > (1UL << 30)) break;   /* huge reservation: assume untouched */
					uint64_t ppa = arch_vm_get_paddr(p->vm->root, x);
					if (ppa) vm_map(p->vm, x, ppa, vmflags);
				}
				va = (end < addr + len ? end : addr + len) - PAGE_SIZE;
				continue;
			}
			if (cgroup_charge_mem(p->vm, p->cgroup, PAGE_SIZE) != 0) return -ENOMEM;
			void *pg = buddy_alloc(0);
			if (!pg) { cgroup_uncharge_mem(p->vm, PAGE_SIZE); return -ENOMEM; }
			memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
			vm_map(p->vm, va, (uint64_t)pg, vmflags);
		}
	}

	/* vm_map only flushed the local TLB; a sibling thread on another CPU keeps a
	 * stale (e.g. still-writable) entry for the reprotected range. Shoot them
	 * down so the new protection takes effect everywhere. */
	arch_tlb_flush_range(p->vm, addr, addr + len);

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
	for (vma_t *v = p->vm->vmas; v; v = v->next)
		if (va >= v->vaddr && va + HUGE_PAGE_SIZE <= v->vaddr + v->size) { vma = v; break; }
	if (!vma) return;
	if (!(vma->flags & VMA_ANON) || !(vma->flags & VMA_WRITE) || (vma->flags & VMA_SHARED))
		return;

	uint64_t pa0 = arch_vm_get_paddr(p->vm->root, va);
	if (!pa0) return;
	for (int j = 0; j < 512; j++) {
		uint64_t pa = arch_vm_get_paddr(p->vm->root, va + (uint64_t)j * PAGE_SIZE);
		if (!pa) return;
		page_t *pg = paddr_to_page(pa);
		if (!pg || pg->refcount != 1) return;   /* absent or shared → skip */
	}

	if (cgroup_charge_mem(p->vm, p->cgroup, HUGE_PAGE_SIZE) != 0) return;
	void *blk = buddy_alloc(9);   /* 2 MB */
	if (!blk) { cgroup_uncharge_mem(p->vm, HUGE_PAGE_SIZE); return; }
	for (int j = 0; j < 512; j++) {
		uint64_t pa = arch_vm_get_paddr(p->vm->root, va + (uint64_t)j * PAGE_SIZE);
		memcpy((void *)PHYS_TO_VIRT((uint64_t)blk + (uint64_t)j * PAGE_SIZE),
		       (void *)PHYS_TO_VIRT(pa), PAGE_SIZE);
	}
	for (uint64_t v = va; v < va + HUGE_PAGE_SIZE; v += PAGE_SIZE)
		vm_unmap(p->vm, v);               /* frees the old 4K pages + empties the table */
	if (vm_map_huge(p->vm, va, (uint64_t)blk, VM_USER | VM_READ | VM_WRITE) != 0) {
		buddy_free((uint64_t)blk, 9);
		cgroup_uncharge_mem(p->vm, HUGE_PAGE_SIZE);
	}
}

/* Locate the anonymous VMA covering [va, va+PAGE_SIZE). Huge VMAs are excluded:
 * they are eagerly populated and never carry a 4K demand-fault path, so the
 * per-page prefault/drop helpers must leave them alone. *end (if asked) is where
 * the VMA holding va ends, anonymous or not. */
static uint32_t anon_vma_at(process_t *p, uint64_t va, uint64_t *end)
{
	uint32_t r = 0;
	uint64_t cf = spin_lock_irq(&p->vm->fault_lock);   /* siblings unmap concurrently */
	for (vma_t *v = p->vm->vmas; v; v = v->next) {
		if (va < v->vaddr || va >= v->vaddr + v->size) continue;
		if ((v->flags & VMA_ANON) && !(v->flags & (VMA_HUGE | VMA_HUGE_1G))) r = v->flags;
		if (end) *end = v->vaddr + v->size;
		break;
	}
	spin_unlock_irq(&p->vm->fault_lock, cf);
	return r;
}

/* Last page before the next VMA above va, for a caller's va += PAGE_SIZE: a range
 * of empty address space must not be walked page by page. No VMA above: stops
 * the caller by landing past every user address. */
static uint64_t vma_next_after(process_t *p, uint64_t va)
{
	uint64_t next = (uint64_t)-1 & ~(uint64_t)(PAGE_SIZE - 1);
	uint64_t cf = spin_lock_irq(&p->vm->fault_lock);
	for (vma_t *v = p->vm->vmas; v; v = v->next) {
		if (v->vaddr > va && v->vaddr < next) next = v->vaddr;
		else if (v->vaddr <= va && v->vaddr + v->size > va && v->vaddr + v->size < next)
			next = v->vaddr + v->size;
	}
	spin_unlock_irq(&p->vm->fault_lock, cf);
	return next - PAGE_SIZE;
}

/* Prefault every absent page of an anonymous range with a zero page carrying
 * the VMA's permissions — the work MADV_WILLNEED and mlock want done up front so
 * a later touch never faults. Best-effort: a dry buddy just stops early. */
static void mm_prefault(process_t *p, uint64_t addr, size_t len)
{
	uint64_t start = addr & ~(uint64_t)(PAGE_SIZE - 1);
	uint64_t end   = (addr + len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	for (uint64_t va = start; va < end; va += PAGE_SIZE) {
		uint32_t vfl = anon_vma_at(p, va, NULL);
		if (!vfl) { va = vma_next_after(p, va); continue; }
		if (!(vfl & VMA_READ)) continue;
		if (arch_vm_get_paddr(p->vm->root, va)) continue;
		if (cgroup_charge_mem(p->vm, p->cgroup, PAGE_SIZE) != 0) return;
		void *pg = mm_alloc_page_oom();
		if (!pg) { cgroup_uncharge_mem(p->vm, PAGE_SIZE); return; }
		memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
		uint32_t mf = VM_USER | VM_READ;
		if (vfl & VMA_WRITE) mf |= VM_WRITE;
		if (vfl & VMA_EXEC)  mf |= VM_EXEC;
		if (vm_map(p->vm, va, (uint64_t)pg, mf) != 0) { buddy_free((uint64_t)pg, 0); cgroup_uncharge_mem(p->vm, PAGE_SIZE); return; }
	}
}

/* Drop the present pages of a private anonymous range (MADV_DONTNEED / MADV_FREE).
 * The pages are freed, which reverses the charge; the next access demand-faults a
 * fresh zero page (vm_handle_fault). Shared anon is left intact — its pages back
 * other mappings — matching MADV_DONTNEED's no-op on MAP_SHARED. We lack reclaim,
 * so MADV_FREE drops eagerly rather than lazily; a re-read still reads as zero,
 * which is a permitted MADV_FREE outcome. */
static void mm_drop(process_t *p, uint64_t addr, size_t len)
{
	uint64_t start = addr & ~(uint64_t)(PAGE_SIZE - 1);
	uint64_t end   = (addr + len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
	/* a VMA at a time, and only its populated page tables (unmap_span): V8
	 * drops gigabytes of mostly empty reservation in one call */
	for (uint64_t va = start; va < end; ) {
		uint64_t vend = 0;
		uint32_t vfl = anon_vma_at(p, va, &vend);
		if (!vfl) { va = vma_next_after(p, va) + PAGE_SIZE; continue; }
		uint64_t hi = vend < end ? vend : end;
		if (!(vfl & VMA_SHARED)) unmap_span(p, va, hi);
		va = hi;
	}
}

/* One memory node: any policy places pages where they would go anyway, so setting
 * one only needs validating. libnuma reads the node mask through these. */
#define MPOL_MAX 7
static long sys_mbind(int mode)
{
	return (mode < 0 || (mode & 0xFFFF) >= MPOL_MAX) ? -22 : 0;
}

static long sys_set_mempolicy(int mode)
{
	return (mode < 0 || (mode & 0xFFFF) >= MPOL_MAX) ? -22 : 0;
}

#define MPOL_F_NODE          1
#define MPOL_F_ADDR          2
#define MPOL_F_MEMS_ALLOWED  4
static long sys_get_mempolicy(int *mode, unsigned long *nmask, unsigned long maxnode, unsigned long flags)
{
	if (flags & ~(unsigned long)(MPOL_F_NODE | MPOL_F_ADDR | MPOL_F_MEMS_ALLOWED)) return -EINVAL;
	if (nmask && maxnode == 0) return -EINVAL;
	if (mode) {
		int m = 0;
		if (copy_to_user(mode, &m, sizeof(m)) != 0) return -EFAULT;
	}
	if (nmask) {
		unsigned long words = (maxnode - 1) / (8 * sizeof(unsigned long)) + 1;
		if (words > 128) words = 128;
		for (unsigned long i = 0; i < words; i++) {
			unsigned long v = (i == 0 && (flags & MPOL_F_MEMS_ALLOWED)) ? 1 : 0;
			if (copy_to_user(nmask + i, &v, sizeof(v)) != 0) return -EFAULT;
		}
	}
	return 0;
}

static long sys_madvise(uint64_t addr, size_t len, int advice)
{
	process_t *p = task_current();
	if (addr & (PAGE_SIZE - 1)) return -EINVAL;
	if (len >= USER_SPAN || !access_ok((void *)addr, len)) return -ENOMEM;
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
	uint64_t cf = spin_lock_irq(&p->vm->fault_lock);
	for (vma_t *v = p->vm->vmas; v; v = v->next) {
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
			if (vstart < lo) vma_add_locked(p->vm, vstart, lo - vstart, oldf);
			if (hi < vend)   vma_add_locked(p->vm, hi, vend - hi, oldf);
			v->vaddr = lo; v->size = hi - lo; v->flags = newf;
		}
	}
	spin_unlock_irq(&p->vm->fault_lock, cf);
}

/* Linux do_mlock: the range must be mapped, and the space's locked bytes after
 * the call (what is already locked inside the range counts once) must fit
 * RLIMIT_MEMLOCK. */
static long mlock_check(process_t *p, uint64_t start, size_t len)
{
	uint64_t end = start + ((len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1));
	if (!range_mapped(p, start, end)) return -ENOMEM;
	uint64_t lim = memlock_limit(p);
	if (lim == RLIM_INFINITY) return 0;
	if (lim == 0) return -EPERM;
	uint64_t inside;
	uint64_t total = locked_bytes(p, start, end, &inside);
	return total - inside + (end - start) > lim ? -ENOMEM : 0;
}

static long sys_mlock(uint64_t addr, size_t len)
{
	if (len == 0) return 0;
	process_t *p = task_current();
	uint64_t start = addr & ~(uint64_t)(PAGE_SIZE - 1);
	len = (addr - start) + len;
	if (!access_ok((void *)start, len)) return -ENOMEM;   /* range outside user space */
	long r = mlock_check(p, start, len);
	if (r) return r;
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
	if (!access_ok((void *)start, len)) return -ENOMEM;
	if (!range_mapped(p, start, start + ((len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1))))
		return -ENOMEM;
	mm_set_locked(p, start, len, 0);
	return 0;
}

#define MLOCK_ONFAULT 0x01

static long sys_mlock2(uint64_t addr, size_t len, int flags)
{
	if (flags & ~MLOCK_ONFAULT) return -EINVAL;
	if (flags & MLOCK_ONFAULT) {   /* lock present pages now, lock the rest as they fault */
		if (len == 0) return 0;
		process_t *p = task_current();
		uint64_t start = addr & ~(uint64_t)(PAGE_SIZE - 1);
		len = (addr - start) + len;
		if (!access_ok((void *)start, len)) return -ENOMEM;
		long r = mlock_check(p, start, len);
		if (r) return r;
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
		return -EINVAL;
	process_t *p = task_current();
	/* Linux: EPERM at limit 0; MCL_CURRENT needs everything mapped to fit */
	uint64_t lim = memlock_limit(p);
	if (lim == 0) return -EPERM;
	if ((flags & MCL_CURRENT) && lim != RLIM_INFINITY && mapped_bytes(p) > lim) return -ENOMEM;
	p->mlock_future = (flags & MCL_FUTURE) ? 1 : 0;
	if (flags & MCL_CURRENT) {
		mm_set_locked(p, 0, USER_SPAN, 1);
		if (!(flags & MCL_ONFAULT)) mm_prefault(p, 0, USER_SPAN);
	}
	return 0;
}

static long sys_munlockall(void)
{
	process_t *p = task_current();
	p->mlock_future = 0;
	mm_set_locked(p, 0, USER_SPAN, 0);
	return 0;
}

static long sys_msync(uint64_t addr, size_t len, int flags)
{
	/* MS_ASYNC=1 MS_INVALIDATE=2 MS_SYNC=4; SYNC and ASYNC are mutually exclusive */
	if (flags & ~0x7) return -EINVAL;
	if ((flags & 1) && (flags & 4)) return -EINVAL;
	if (addr & 0xFFF) return -EINVAL;   /* must be page-aligned */
	process_t *p = task_current();
	uint64_t end = addr + ((len + 0xFFF) & ~0xFFFULL);
	/* the whole range must be mapped, else ENOMEM */
	for (uint64_t va = addr; va < end; va += 0x1000) {
		int found = 0;
		for (vma_t *v = p->vm->vmas; v; v = v->next)
			if (va >= v->vaddr && va < v->vaddr + v->size) { found = 1; break; }
		if (!found) return -ENOMEM;
	}
	if (flags & 5) filecopy_sync(p->vm, addr, end - addr);   /* MS_SYNC, MS_ASYNC */
#if IS_ENABLED(CONFIG_FS_EXT2)
	if (flags & 4) ext2_sync_all();   /* the disk cache is write-back */
#endif
	return 0;
}
