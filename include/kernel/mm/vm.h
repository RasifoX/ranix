/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef VM_H
#define VM_H

#include <uapi/types.h>
#include <kernel/sync.h>
#include <asm/mmu.h>   /* the user layout (HUGE1G window, VA end) is per arch */

#define VM_READ   (1 << 0)
#define VM_WRITE  (1 << 1)
#define VM_EXEC   (1 << 2)
#define VM_USER   (1 << 3)
#define VM_NOCACHE (1 << 4)
#define VM_DEVICE  (1 << 5)
#define VM_WC      (1 << 12)   /* write-combining: x86 only, else as VM_NOCACHE */
#define VM_NOFREE  (1 << 6)   /* kernel-shared page (vDSO/vvar, rings): unmap, never free */

#define HUGE_PAGE_SIZE  (2UL * 1024 * 1024)
#define HUGE_PAGE_MASK  (~(HUGE_PAGE_SIZE - 1))

/* 1 GB hugepage mmaps live in their own high VA window, above the mmap ceiling
 * and below the stack, so a 1 GB-aligned block never collides with the normal
 * mmap region or the stack. Stays inside page-table slot 0 (< 512 GB). */
#ifdef ARCH_HUGE1G_VA_BASE
#define HUGE1G_VA_BASE  ARCH_HUGE1G_VA_BASE
#else
#define HUGE1G_VA_BASE  0x3F00000000UL   /* 252 GB */
#endif

#define VM_SPACE_MAGIC 0x564D5350u   /* "VMSP" — set at create, checked at lifecycle ops */

typedef struct {
	uint64_t  *root;       /* physical address of top-level page table */
	uint32_t   refcount;   /* number of processes/threads sharing this space */
	uint32_t   asid;       /* aarch64 TLB tag (1..255; 0 = flush-on-switch); unused on x86 */
	spinlock_t fault_lock; /* serializes fault resolution + the cursors below among shared threads */
	struct vma *vmas;      /* mapped-region list; shared by all threads of this space */
	uint64_t   mmap_next;  /* bump cursor for anon/file mmap placement (shared by all threads) */
	uint64_t   huge1g_next;/* bump cursor for the 1 GB-hugepage VA window (shared by all threads) */
	uint64_t   brk;        /* program break: one per address space, not per task */
	mutex_t    brk_lock;   /* serializes brk; it allocates, so it cannot hold fault_lock */
	void      *mem_cg;     /* cgroup holding mem_charged, set by the first charge */
	long       mem_charged;/* bytes this space asked for; the OOM killer's score */
	long       pt_charged; /* of which page tables, freed with the space */
	uint64_t   swap_scan;  /* reclaim's clock hand: the next VA it looks at (kernel/mm/reclaim.c) */
	uint32_t   locked_all; /* mlockall: no page of the space is swapped, heap and stack too */
	uint32_t   test_hook;  /* PR_RANIX_TEST_HOOK, test kernels only (kernel/mm/reclaim.c) */
	uint32_t   magic;      /* VM_SPACE_MAGIC; corruption/use-after-free tripwire */
} vm_space_t;

extern vm_space_t *kernel_vm_space;

vm_space_t *vm_create(void);
vm_space_t *vm_copy(vm_space_t *vs);
vm_space_t *vm_get(vm_space_t *vs);   /* increment refcount */
void        vm_destroy(vm_space_t *vs); /* decrement refcount; free when 0 */
int         vm_map(vm_space_t *vs, uint64_t vaddr, uint64_t paddr, uint32_t flags);
int         vm_map_huge(vm_space_t *vs, uint64_t vaddr, uint64_t paddr, uint32_t flags);
int         vm_map_huge_1g(vm_space_t *vs, uint64_t vaddr, uint64_t paddr, uint32_t flags);
void        vm_unmap(vm_space_t *vs, uint64_t vaddr);
uint64_t    vm_skip_empty(vm_space_t *vs, uint64_t vaddr);   /* next VA whose page table exists */
void        vm_activate(void);
void        vm_set_kernel_pt(uint64_t phys);

void        vm_init_kernel(uint64_t root_phys);
void       *ioremap(uint64_t phys_addr, size_t size, uint32_t flags);
int         vm_handle_fault(uint64_t vaddr, int is_write);   /* as from interrupts off: never sleeps */
int         vm_handle_fault_irq(uint64_t vaddr, int is_write, int irqs_on);
void        vm_fault_quiet(int on);   /* no error log for an expected kernel fault */
size_t      vm_access_remote(vm_space_t *vs, uint64_t va, void *kbuf, size_t n, int write);
/* A page of the caller's space held for a device that reaches it by physical
 * address (KVM guest memory): physical address or 0. Unpin with the same space. */
uint64_t    vm_pin_user_page(uint64_t va, int write);
void        vm_unpin_user_page(vm_space_t *vs, uint64_t pa);

/* arch-specific: load page table into hardware. asid is the address space's
 * TLB tag on aarch64 (0 forces a flush); ignored on x86_64. */
void arch_vm_switch(uint64_t root_phys, uint32_t asid);

/* Cross-CPU TLB shootdown after a PTE downgrade (unmap / COW write-protect /
 * mprotect). On x86_64 a software IPI flushes the other online CPUs; on
 * aarch64 the broadcast TLBI in vm.c already covers all cores, so this is a
 * no-op. Must NOT be called while holding vs->fault_lock. */
void arch_tlb_flush_range(vm_space_t *vs, uint64_t start, uint64_t end);
/* before freeing a page vs mapped: 1 if other CPUs must be shot down; other
 * CPUs that only kept vs's entries are made to reload instead (vm.c) */
int  vm_tlb_elsewhere(vm_space_t *vs);

uint64_t arch_vm_get_paddr(uint64_t *root, uint64_t vaddr);

/* Leaf (4 KB) page-table entries, for swap (kernel/mm/swap.c). Callers hold
 * vs->fault_lock. arch_pte_next: the entry for the first page at or above *va
 * (moved there) below end whose page table exists, else NULL. arch_pte_set
 * stores an entry and drops this CPU's TLB entry; the caller shoots down the
 * others after the lock. arch_pte_young tests and clears the accessed bit.
 * arch_pte_page: the page of a present 4 KB user entry the kernel may take
 * away (not VM_NOFREE), else 0. */
uint64_t *arch_pte_next(vm_space_t *vs, uint64_t *va, uint64_t end);
uint64_t *arch_pte_find(vm_space_t *vs, uint64_t va);
void      arch_pte_set(vm_space_t *vs, uint64_t va, uint64_t *pte, uint64_t val);
int       arch_pte_young(uint64_t *pte);
uint64_t  arch_pte_page(uint64_t e);

/* vm.mmap_min_addr: nothing is mapped below it for a task without
 * CAP_SYS_RAWIO, so a kernel NULL dereference never lands on user memory */
#define MMAP_MIN_ADDR 65536UL
void vma_free_space(vm_space_t *vs);

#endif
