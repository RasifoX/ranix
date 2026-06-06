#include <kernel/mm/vm.h>
#include <kernel/sync.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/hugetlb.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/bug.h>
#include <asm/mmu.h>

#define PT_ENTRIES  512
#define PTE_P   (1UL << 0)
#define PTE_W   (1UL << 1)
#define PTE_U   (1UL << 2)
#define PTE_PWT (1UL << 3)
#define PTE_PCD (1UL << 4)
#define PTE_PS  (1UL << 7)
#define PTE_NOFREE (1UL << 9)   /* software bit: kernel-shared page, skip free on vm_destroy */
#define PTE_NX  (1UL << 63)     /* no-execute (EFER.NXE); cleared only when VM_EXEC */

/* Physical-address field of a PTE (bits 51:12). Masks off both the low flag
 * bits and the high attribute bits — notably PTE_NX at bit 63 — so a leaf
 * entry's address survives extraction. */
#define PA_MASK     0x000FFFFFFFFFF000UL
#define PA_2M_MASK  0x000FFFFFFFE00000UL
#define PA_1G_MASK  0x000FFFFFC0000000UL

extern uint64_t pml4[];      /* kernel (boot) PML4 — phys when cast to uint64_t */

/* 5-level paging flag, cached in the high half. The raw boot symbol (la57)
 * lives at a low VA reachable only through the identity map (pml4[0]), which user
 * address spaces don't carry — reading it during a fault in user context would
 * fault again. mmu_init() seeds this while the identity map is still live. */
static int la57;
void arch_vm_set_la57(int v) { la57 = v; }

/* PML5 slots holding the kernel's 48-bit-canonical mappings under 5-level. */
#define PML5_DMAP  504   /* direct map  (0xFFFF800000000000) */
#define PML5_KTEXT 511   /* kernel text (0xFFFFFFFF80000000) */

static uint64_t alloc_table(void)
{
	void *p = buddy_alloc(0);
	if (!p) return 0;
	memset((void *)PHYS_TO_VIRT(p), 0, PAGE_SIZE);
	return (uint64_t)p;
}

/* The PML4 the CPU reaches for user VA. In 5-level it lives under pml5[0], so
 * the rest of every walk (PML4→PDPT→PD→PT) is identical to 4-level. */
static uint64_t user_pml4(vm_space_t *vs)
{
	if (la57) {
		uint64_t *pml5 = (uint64_t *)PHYS_TO_VIRT((uint64_t)vs->root);
		return pml5[0] & ~0xFFFUL;
	}
	return (uint64_t)vs->root;
}

vm_space_t *vm_create(void)
{
	void *vs_phys = buddy_alloc(0);
	if (!vs_phys) return NULL;
	vm_space_t *vs = (vm_space_t *)PHYS_TO_VIRT(vs_phys);
	vs->root = NULL;
	vs->fault_lock = (spinlock_t)SPINLOCK_INIT;

	uint64_t pml4_phys = alloc_table();
	if (!pml4_phys) { buddy_free(VIRT_TO_PHYS(vs), 0); return NULL; }

	if (la57) {
		/* User PML4 is purely user (empty); kernel mappings come from the
		 * shared boot PML4 via pml5[504]/[511]. */
		uint64_t pml5_phys = alloc_table();
		if (!pml5_phys) { buddy_free(pml4_phys, 0); buddy_free(VIRT_TO_PHYS(vs), 0); return NULL; }
		uint64_t *pml5 = (uint64_t *)PHYS_TO_VIRT(pml5_phys);
		pml5[0]          = pml4_phys      | PTE_P | PTE_W | PTE_U;
		pml5[PML5_DMAP]  = (uint64_t)pml4 | PTE_P | PTE_W;
		pml5[PML5_KTEXT] = (uint64_t)pml4 | PTE_P | PTE_W;
		vs->root = (uint64_t *)pml5_phys;
	} else {
		uint64_t *root_virt  = (uint64_t *)PHYS_TO_VIRT(pml4_phys);
		uint64_t *kpml4_virt = (uint64_t *)PHYS_TO_VIRT(pml4);
		for (int i = 1; i < PT_ENTRIES; i++)
			root_virt[i] = kpml4_virt[i];
		vs->root = (uint64_t *)pml4_phys;
	}

	vs->refcount = 1;
	vs->magic = VM_SPACE_MAGIC;
	return vs;
}

vm_space_t *vm_get(vm_space_t *vs)
{
	/* Atomic: threads share a vm; concurrent create/exit on different CPUs
	 * would otherwise race this refcount and free the page tables early. */
	if (vs) { KASSERT(vs->magic == VM_SPACE_MAGIC); atomic_fetch_add32(&vs->refcount, 1); }
	return vs;
}

void vm_destroy(vm_space_t *vs)
{
	if (!vs) return;
	KASSERT(vs->magic == VM_SPACE_MAGIC);
	if (atomic_sub_fetch32(&vs->refcount, 1) > 0) return;

	if (!vs->root) { vs->magic = 0; buddy_free(VIRT_TO_PHYS(vs), 0); return; }

	uint64_t l4_phys = user_pml4(vs);   /* under pml5[0] in 5-level */
	uint64_t *l4 = (uint64_t *)PHYS_TO_VIRT(l4_phys);

	/* We only walk index 0 (user space) */
	for (int i = 0; i < 1; i++) {
		if (!(l4[i] & PTE_P)) continue;

		uint64_t l3_phys = l4[i] & ~0xFFFUL;
		uint64_t *l3 = (uint64_t *)PHYS_TO_VIRT(l3_phys);
		for (int j = 0; j < 512; j++) {
			if (!(l3[j] & PTE_P)) continue;
			if (l3[j] & PTE_PS) {
				if (l3[j] & PTE_U) huge1g_put(l3[j] & PA_1G_MASK);  /* 1GB block */
				continue;
			}

			uint64_t l2_phys = l3[j] & ~0xFFFUL;
			uint64_t *l2 = (uint64_t *)PHYS_TO_VIRT(l2_phys);
			for (int k = 0; k < 512; k++) {
				if (!(l2[k] & PTE_P)) continue;
				if (l2[k] & PTE_PS) {
					if (l2[k] & PTE_U) {
						huge2m_account(-1);
						uint64_t pa = l2[k] & PA_2M_MASK;
						for (int l = 0; l < 512; l++)
							pmm_free((void *)(pa + (uint64_t)l * PAGE_SIZE));
					}
					continue;
				}

				uint64_t l1_phys = l2[k] & ~0xFFFUL;
				uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT(l1_phys);
				for (int l = 0; l < 512; l++) {
					if (!(l1[l] & PTE_P)) continue;
					if ((l1[l] & PTE_U) && !(l1[l] & PTE_NOFREE)) {
						pmm_free((void *)(l1[l] & PA_MASK));
					}
				}
				buddy_free(l1_phys, 0);
			}
			buddy_free(l2_phys, 0);
		}
		buddy_free(l3_phys, 0);
	}

	buddy_free(l4_phys, 0);                                  /* user PML4 */
	if (la57) buddy_free((uint64_t)vs->root, 0);     /* PML5 */
	vs->magic = 0;                                   /* poison before free: trips a later use-after-free */
	buddy_free(VIRT_TO_PHYS(vs), 0);
}

static uint64_t get_or_alloc(uint64_t table_phys, uint64_t idx, uint64_t eflags)
{
	uint64_t *table = (uint64_t *)PHYS_TO_VIRT(table_phys);
	if (table[idx] & PTE_P) {
		if (table[idx] & PTE_PS) {
			/* split 2MB huge page into 512 4KB pages */
			uint64_t base = table[idx] & PA_2M_MASK;
			uint64_t attr = table[idx] & (PTE_P | PTE_W | PTE_U | PTE_NX);
			uint64_t pt_phys  = alloc_table();
			if (!pt_phys) return 0;
			uint64_t *pt_virt = (uint64_t *)PHYS_TO_VIRT(pt_phys);
			for (int j = 0; j < PT_ENTRIES; j++)
				pt_virt[j] = (base + (uint64_t)j * 0x1000) | attr;
			table[idx] = pt_phys | PTE_P | PTE_W | PTE_U;
			/* flush TLB — whole CR3 reload since we're changing a huge entry */
			__asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
			return pt_phys;
		}
		/* propagate PTE_U flag to intermediate tables */
		table[idx] |= eflags & PTE_U;
		return table[idx] & ~0xFFFUL;
	}
	uint64_t child_phys = alloc_table();
	if (!child_phys) return 0;
	table[idx] = child_phys | eflags;
	return child_phys;
}

int vm_map(vm_space_t *vs, uint64_t vaddr, uint64_t paddr, uint32_t flags)
{
	uint64_t eflags = PTE_P | PTE_W | PTE_U;

	uint64_t pdp_phys = get_or_alloc(user_pml4(vs), (vaddr >> 39) & 0x1FF, eflags);
	if (!pdp_phys) return -1;
	uint64_t pd_phys  = get_or_alloc(pdp_phys,           (vaddr >> 30) & 0x1FF, eflags);
	if (!pd_phys)  return -1;
	uint64_t pt_phys  = get_or_alloc(pd_phys,            (vaddr >> 21) & 0x1FF, eflags);
	if (!pt_phys)  return -1;

	uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pt_phys);
	uint64_t entry = (paddr & ~0xFFFUL) | PTE_P;
	if (flags & VM_WRITE)   entry |= PTE_W;
	if (flags & VM_USER)    entry |= PTE_U;
	if (flags & VM_NOCACHE) entry |= PTE_PWT | PTE_PCD;
	if (flags & VM_DEVICE)  entry |= PTE_PWT | PTE_PCD;
	if (flags & VM_NOFREE)  entry |= PTE_NOFREE;   /* shared page: don't free on teardown */
	if (!(flags & VM_EXEC)) entry |= PTE_NX;
	pt[(vaddr >> 12) & 0x1FF] = entry;

	__asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
	return 0;
}

/* Map a 2 MB block at the PD level. paddr must be 2 MB-aligned. */
int vm_map_huge(vm_space_t *vs, uint64_t vaddr, uint64_t paddr, uint32_t flags)
{
	uint64_t eflags = PTE_P | PTE_W | PTE_U;

	uint64_t pdp_phys = get_or_alloc(user_pml4(vs), (vaddr >> 39) & 0x1FF, eflags);
	if (!pdp_phys) return -1;
	uint64_t pd_phys  = get_or_alloc(pdp_phys,           (vaddr >> 30) & 0x1FF, eflags);
	if (!pd_phys)  return -1;

	uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pd_phys);
	uint64_t old = pd[(vaddr >> 21) & 0x1FF];
	uint64_t entry = (paddr & ~0x1FFFFFUL) | PTE_P | PTE_PS;
	if (flags & VM_WRITE)   entry |= PTE_W;
	if (flags & VM_USER)    entry |= PTE_U;
	if (flags & VM_NOCACHE) entry |= PTE_PWT | PTE_PCD;
	if (!(flags & VM_EXEC)) entry |= PTE_NX;
	pd[(vaddr >> 21) & 0x1FF] = entry;
	/* replacing a 4K leaf table (THP collapse): reclaim the now-orphaned table */
	if ((old & PTE_P) && !(old & PTE_PS)) buddy_free(old & ~0xFFFUL, 0);
	if (flags & VM_USER) huge2m_account(1);

	__asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
	return 0;
}

/* Map a 1 GB block at the PDPT level. paddr must be 1 GB-aligned. */
int vm_map_huge_1g(vm_space_t *vs, uint64_t vaddr, uint64_t paddr, uint32_t flags)
{
	uint64_t eflags = PTE_P | PTE_W | PTE_U;

	uint64_t pdp_phys = get_or_alloc(user_pml4(vs), (vaddr >> 39) & 0x1FF, eflags);
	if (!pdp_phys) return -1;

	uint64_t *pdp = (uint64_t *)PHYS_TO_VIRT(pdp_phys);
	uint64_t entry = (paddr & ~0x3FFFFFFFUL) | PTE_P | PTE_PS;
	if (flags & VM_WRITE)   entry |= PTE_W;
	if (flags & VM_USER)    entry |= PTE_U;
	if (flags & VM_NOCACHE) entry |= PTE_PWT | PTE_PCD;
	if (!(flags & VM_EXEC)) entry |= PTE_NX;
	pdp[(vaddr >> 30) & 0x1FF] = entry;

	__asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
	return 0;
}

void vm_unmap(vm_space_t *vs, uint64_t vaddr)
{
	/* Serialize against concurrent faults in a shared address space. */
	uint64_t lf  = spin_lock_irq(&vs->fault_lock);
	uint64_t pte = 0;

	uint64_t huge_base = 0, huge1g = 0;
	uint64_t *l4 = (uint64_t *)PHYS_TO_VIRT(user_pml4(vs));
	if (l4[(vaddr >> 39) & 0x1FF] & PTE_P) {
		uint64_t *pdp = (uint64_t *)PHYS_TO_VIRT(l4[(vaddr >> 39) & 0x1FF] & ~0xFFFUL);
		uint64_t pdpe = pdp[(vaddr >> 30) & 0x1FF];
		if ((pdpe & PTE_P) && (pdpe & PTE_PS)) {
			/* 1 GB block */
			if (pdpe & PTE_U) huge1g = pdpe & PA_1G_MASK;
			pdp[(vaddr >> 30) & 0x1FF] = 0;
			__asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
		} else if (pdpe & PTE_P) {
			uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpe & ~0xFFFUL);
			uint64_t pde = pd[(vaddr >> 21) & 0x1FF];
			if ((pde & PTE_P) && (pde & PTE_PS)) {
				if (pde & PTE_U) huge_base = pde & PA_2M_MASK;
				pd[(vaddr >> 21) & 0x1FF] = 0;
				__asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
			} else if (pde & PTE_P) {
				uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pde & ~0xFFFUL);
				pte = pt[(vaddr >> 12) & 0x1FF];
				pt[(vaddr >> 12) & 0x1FF] = 0;
				__asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
			}
		}
	}

	spin_unlock_irq(&vs->fault_lock, lf);

	/* Force the other CPUs to drop their stale entry for this page BEFORE the
	 * page is freed/reused — a sibling thread in the same address space may
	 * still have a writable TLB entry. Done outside fault_lock so a sibling
	 * spinning on it (IRQ-off) can still ack the shootdown IPI. */
	if (pte || huge_base || huge1g)
		arch_tlb_flush_range(vs, vaddr, vaddr + PAGE_SIZE);

	if (huge1g) {
		huge1g_put(huge1g);
	} else if (huge_base) {
		huge2m_account(-1);
		for (int i = 0; i < 512; i++)
			pmm_free((void *)(huge_base + (uint64_t)i * PAGE_SIZE));
	} else if ((pte & PTE_P) && (pte & PTE_U))
		pmm_free((void *)(pte & PA_MASK));
}

uint64_t arch_vm_get_paddr(uint64_t *pml4_phys, uint64_t vaddr)
{
	/* Callers pass vs->root; under 5-level that's the PML5 — descend pml5[0]. */
	if (la57)
		pml4_phys = (uint64_t *)(((uint64_t *)PHYS_TO_VIRT((uint64_t)pml4_phys))[0] & ~0xFFFUL);
	uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(pml4_phys);
	uint64_t p4i = (vaddr >> 39) & 0x1FF;
	uint64_t p3i = (vaddr >> 30) & 0x1FF;
	uint64_t p2i = (vaddr >> 21) & 0x1FF;
	uint64_t p1i = (vaddr >> 12) & 0x1FF;

	if (!(pml4[p4i] & PTE_P)) return 0;
	uint64_t *pdp = (uint64_t *)PHYS_TO_VIRT(pml4[p4i] & ~0xFFFUL);

	if (!(pdp[p3i] & PTE_P)) return 0;
	if (pdp[p3i] & PTE_PS) return (pdp[p3i] & PA_1G_MASK) | (vaddr & 0x3FFFFFFFUL);

	uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdp[p3i] & ~0xFFFUL);
	if (!(pd[p2i] & PTE_P)) return 0;
	if (pd[p2i] & PTE_PS) return (pd[p2i] & PA_2M_MASK) | (vaddr & 0x1FFFFFUL);

	uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[p2i] & ~0xFFFUL);
	if (!(pt[p1i] & PTE_P)) return 0;
	return (pt[p1i] & PA_MASK);
}

/* Shoot down the other CPUs after a PTE downgrade. invlpg/CR3 are CPU-local,
 * so a sibling thread on another CPU in the same address space keeps a stale
 * TLB entry until forced to flush. range is currently flush-everything (a CR3
 * reload on each target); per-page invlpg can come later if it matters. */
void arch_tlb_flush_range(vm_space_t *vs, uint64_t start, uint64_t end)
{
	(void)vs; (void)start; (void)end;
	extern void arch_tlb_shootdown_all(void);
	arch_tlb_shootdown_all();
}

void arch_vm_switch(uint64_t root_phys, uint32_t asid)
{
	(void)asid;   /* x86_64 flushes non-global TLB on CR3 reload; PCID is later */
	/* Skip the CR3 reload (which flushes the non-global TLB) when the
	 * address space is unchanged — avoids needless flushes on thread
	 * switches and same-task re-runs. */
	uint64_t cur;
	__asm__ volatile("mov %%cr3, %0" : "=r"(cur));
	if ((cur & ~0xFFFUL) == (root_phys & ~0xFFFUL))
		return;
	__asm__ volatile("mov %0, %%cr3" :: "r"(root_phys) : "memory");
}

vm_space_t *vm_copy(vm_space_t *vs)
{
	void *new_vs_phys = buddy_alloc(0);
	if (!new_vs_phys) return NULL;
	vm_space_t *new_vs = (vm_space_t *)PHYS_TO_VIRT(new_vs_phys);
	new_vs->root = NULL;
	new_vs->refcount = 1;
	new_vs->fault_lock = (spinlock_t)SPINLOCK_INIT;
	new_vs->magic = VM_SPACE_MAGIC;

	uint64_t root_phys = alloc_table();   /* the new user PML4 */
	if (!root_phys) goto oom;
	uint64_t *l4_new = (uint64_t *)PHYS_TO_VIRT(root_phys);

	if (la57) {
		uint64_t pml5_phys = alloc_table();
		if (!pml5_phys) { buddy_free(root_phys, 0); goto oom; }
		uint64_t *pml5 = (uint64_t *)PHYS_TO_VIRT(pml5_phys);
		pml5[0]          = root_phys      | PTE_P | PTE_W | PTE_U;
		pml5[PML5_DMAP]  = (uint64_t)pml4 | PTE_P | PTE_W;
		pml5[PML5_KTEXT] = (uint64_t)pml4 | PTE_P | PTE_W;
		new_vs->root = (uint64_t *)pml5_phys;   /* user PML4 stays kernel-free */
	} else {
		new_vs->root = (uint64_t *)root_phys;
		uint64_t *kpml4 = (uint64_t *)PHYS_TO_VIRT(pml4);
		for (int i = 1; i < 512; i++)
			l4_new[i] = kpml4[i];
	}

	/* Lock the parent across the walk: sibling threads may fault concurrently,
	 * and vm_copy both reads and write-protects the parent's leaf PTEs. */
	uint64_t lf = spin_lock_irq(&vs->fault_lock);

	uint64_t *l4_old = (uint64_t *)PHYS_TO_VIRT(user_pml4(vs));

	uint64_t eflags = PTE_P | PTE_W | PTE_U;

	for (int i = 0; i < 1; i++) {
		if (!(l4_old[i] & PTE_P)) continue;

		uint64_t *l3_old = (uint64_t *)PHYS_TO_VIRT(l4_old[i] & ~0xFFFUL);
		uint64_t l3_new_phys = alloc_table();
		if (!l3_new_phys) { spin_unlock_irq(&vs->fault_lock, lf); goto oom; }
		uint64_t *l3_new = (uint64_t *)PHYS_TO_VIRT(l3_new_phys);
		l4_new[i] = l3_new_phys | eflags;

		for (int j = 0; j < 512; j++) {
			if (!(l3_old[j] & PTE_P)) continue;
			if (l3_old[j] & PTE_PS) {
				/* 1 GB block: COW it — write-protect both, bump the pool
				 * refcount, copy the entry. A write fault copies the 1 GB. */
				uint64_t entry = l3_old[j];
				if (entry & PTE_U) {
					entry &= ~PTE_W;
					l3_old[j] = entry;
					huge1g_get(entry & PA_1G_MASK);
				}
				l3_new[j] = entry;
				continue;
			}

			uint64_t *l2_old = (uint64_t *)PHYS_TO_VIRT(l3_old[j] & ~0xFFFUL);
			uint64_t l2_new_phys = alloc_table();
			if (!l2_new_phys) { spin_unlock_irq(&vs->fault_lock, lf); goto oom; }
			uint64_t *l2_new = (uint64_t *)PHYS_TO_VIRT(l2_new_phys);
			l3_new[j] = l2_new_phys | eflags;

			for (int k = 0; k < 512; k++) {
				if (!(l2_old[k] & PTE_P)) continue;
				if (l2_old[k] & PTE_PS) {
					uint64_t entry = l2_old[k];
					if (entry & PTE_U) {
						entry &= ~PTE_W;
						l2_old[k] = entry;
						uint64_t pa = entry & PA_2M_MASK;
						for (int l = 0; l < 512; l++)
							pmm_hold((void *)(pa + (uint64_t)l * PAGE_SIZE));
					}
					l2_new[k] = entry;
					continue;
				}

				uint64_t *l1_old = (uint64_t *)PHYS_TO_VIRT(l2_old[k] & ~0xFFFUL);
				uint64_t l1_new_phys = alloc_table();
				if (!l1_new_phys) { spin_unlock_irq(&vs->fault_lock, lf); goto oom; }
				uint64_t *l1_new = (uint64_t *)PHYS_TO_VIRT(l1_new_phys);
				l2_new[k] = l1_new_phys | eflags;

				for (int l = 0; l < 512; l++) {
					if (!(l1_old[l] & PTE_P)) continue;
					uint64_t entry = l1_old[l];
					if ((entry & PTE_U) && !(entry & PTE_NOFREE)) {
						entry &= ~PTE_W;
						l1_old[l] = entry;
						pmm_hold((void *)(entry & PA_MASK));
					}
					l1_new[l] = entry;
				}
			}
		}
	}

	spin_unlock_irq(&vs->fault_lock, lf);

	uint64_t cr3;
	__asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");

	/* The parent's leaf PTEs were write-protected for COW. A sibling thread of
	 * the parent on another CPU still holds writable TLB entries and would write
	 * straight through, breaking copy-on-write. Shoot them down (outside
	 * fault_lock). */
	arch_tlb_flush_range(vs, 0, 0);

	return new_vs;

oom:
	vm_destroy(new_vs);
	return NULL;
}
