#include <kernel/mm/vm.h>
#include <kernel/sync.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <asm/mmu.h>

#define PT_ENTRIES  512
#define PTE_P   (1UL << 0)
#define PTE_W   (1UL << 1)
#define PTE_U   (1UL << 2)
#define PTE_PWT (1UL << 3)
#define PTE_PCD (1UL << 4)
#define PTE_PS  (1UL << 7)

extern uint64_t pml4[];

static uint64_t alloc_table(void)
{
	void *p = buddy_alloc(0);
	if (!p) return 0;
	memset((void *)PHYS_TO_VIRT(p), 0, PAGE_SIZE);
	return (uint64_t)p;
}

vm_space_t *vm_create(void)
{
	void *vs_phys = buddy_alloc(0);
	if (!vs_phys) return NULL;
	vm_space_t *vs = (vm_space_t *)PHYS_TO_VIRT(vs_phys);
	vs->root = NULL;
	vs->fault_lock = (spinlock_t)SPINLOCK_INIT;

	uint64_t root_phys = alloc_table();
	if (!root_phys) { buddy_free(VIRT_TO_PHYS(vs), 0); return NULL; }
	vs->root = (uint64_t *)root_phys;

	uint64_t *root_virt = (uint64_t *)PHYS_TO_VIRT(root_phys);
	uint64_t *kpml4_virt = (uint64_t *)PHYS_TO_VIRT(pml4);

	memset(root_virt, 0, PAGE_SIZE);
	for (int i = 1; i < PT_ENTRIES; i++)
		root_virt[i] = kpml4_virt[i];

	vs->refcount = 1;
	return vs;
}

vm_space_t *vm_get(vm_space_t *vs)
{
	/* Atomic: threads share a vm; concurrent create/exit on different CPUs
	 * would otherwise race this refcount and free the page tables early. */
	if (vs) atomic_fetch_add32(&vs->refcount, 1);
	return vs;
}

void vm_destroy(vm_space_t *vs)
{
	if (!vs) return;
	if (atomic_sub_fetch32(&vs->refcount, 1) > 0) return;

	if (!vs->root) { buddy_free(VIRT_TO_PHYS(vs), 0); return; }

	uint64_t l4_phys = (uint64_t)vs->root;
	uint64_t *l4 = (uint64_t *)PHYS_TO_VIRT(l4_phys);

	/* We only walk index 0 (user space) */
	for (int i = 0; i < 1; i++) {
		if (!(l4[i] & PTE_P)) continue;

		uint64_t l3_phys = l4[i] & ~0xFFFUL;
		uint64_t *l3 = (uint64_t *)PHYS_TO_VIRT(l3_phys);
		for (int j = 0; j < 512; j++) {
			if (!(l3[j] & PTE_P)) continue;
			if (l3[j] & PTE_PS) continue;

			uint64_t l2_phys = l3[j] & ~0xFFFUL;
			uint64_t *l2 = (uint64_t *)PHYS_TO_VIRT(l2_phys);
			for (int k = 0; k < 512; k++) {
				if (!(l2[k] & PTE_P)) continue;
				if (l2[k] & PTE_PS) {
					if (l2[k] & PTE_U) {
						uint64_t pa = l2[k] & ~0x1FFFFFUL;
						for (int l = 0; l < 512; l++)
							pmm_free((void *)(pa + (uint64_t)l * PAGE_SIZE));
					}
					continue;
				}

				uint64_t l1_phys = l2[k] & ~0xFFFUL;
				uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT(l1_phys);
				for (int l = 0; l < 512; l++) {
					if (!(l1[l] & PTE_P)) continue;
					if (l1[l] & PTE_U) {
						pmm_free((void *)(l1[l] & ~0xFFFUL));
					}
				}
				buddy_free(l1_phys, 0);
			}
			buddy_free(l2_phys, 0);
		}
		buddy_free(l3_phys, 0);
	}

	buddy_free(l4_phys, 0);
	buddy_free(VIRT_TO_PHYS(vs), 0);
}

static uint64_t get_or_alloc(uint64_t table_phys, uint64_t idx, uint64_t eflags)
{
	uint64_t *table = (uint64_t *)PHYS_TO_VIRT(table_phys);
	if (table[idx] & PTE_P) {
		if (table[idx] & PTE_PS) {
			/* split 2MB huge page into 512 4KB pages */
			uint64_t base = table[idx] & ~0x1FFFFFUL;
			uint64_t attr = table[idx] & (PTE_P | PTE_W);
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

	uint64_t pdp_phys = get_or_alloc((uint64_t)vs->root, (vaddr >> 39) & 0x1FF, eflags);
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
	pt[(vaddr >> 12) & 0x1FF] = entry;

	__asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
	return 0;
}

void vm_unmap(vm_space_t *vs, uint64_t vaddr)
{
	/* Serialize against concurrent faults in a shared address space. */
	uint64_t lf  = spin_lock_irq(&vs->fault_lock);
	uint64_t pte = 0;

	uint64_t *l4 = (uint64_t *)PHYS_TO_VIRT(vs->root);
	if (l4[(vaddr >> 39) & 0x1FF] & PTE_P) {
		uint64_t *pdp = (uint64_t *)PHYS_TO_VIRT(l4[(vaddr >> 39) & 0x1FF] & ~0xFFFUL);
		if (pdp[(vaddr >> 30) & 0x1FF] & PTE_P) {
			uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdp[(vaddr >> 30) & 0x1FF] & ~0xFFFUL);
			if (pd[(vaddr >> 21) & 0x1FF] & PTE_P) {
				uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[(vaddr >> 21) & 0x1FF] & ~0xFFFUL);
				pte = pt[(vaddr >> 12) & 0x1FF];
				pt[(vaddr >> 12) & 0x1FF] = 0;
				__asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
			}
		}
	}

	spin_unlock_irq(&vs->fault_lock, lf);

	if ((pte & PTE_P) && (pte & PTE_U))
		pmm_free((void *)(pte & ~0xFFFUL));
}

uint64_t arch_vm_get_paddr(uint64_t *pml4_phys, uint64_t vaddr)
{
	uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(pml4_phys);
	uint64_t p4i = (vaddr >> 39) & 0x1FF;
	uint64_t p3i = (vaddr >> 30) & 0x1FF;
	uint64_t p2i = (vaddr >> 21) & 0x1FF;
	uint64_t p1i = (vaddr >> 12) & 0x1FF;

	if (!(pml4[p4i] & PTE_P)) return 0;
	uint64_t *pdp = (uint64_t *)PHYS_TO_VIRT(pml4[p4i] & ~0xFFFUL);

	if (!(pdp[p3i] & PTE_P)) return 0;
	if (pdp[p3i] & PTE_PS) return (pdp[p3i] & ~0x3FFFFFFFUL) | (vaddr & 0x3FFFFFFFUL);

	uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdp[p3i] & ~0xFFFUL);
	if (!(pd[p2i] & PTE_P)) return 0;
	if (pd[p2i] & PTE_PS) return (pd[p2i] & ~0x1FFFFFUL) | (vaddr & 0x1FFFFFUL);

	uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[p2i] & ~0xFFFUL);
	if (!(pt[p1i] & PTE_P)) return 0;
	return (pt[p1i] & ~0xFFFUL);
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

	uint64_t root_phys = alloc_table();
	if (!root_phys) goto oom;
	new_vs->root = (uint64_t *)root_phys;

	/* Lock the parent across the walk: sibling threads may fault concurrently,
	 * and vm_copy both reads and write-protects the parent's leaf PTEs. */
	uint64_t lf = spin_lock_irq(&vs->fault_lock);

	uint64_t *l4_old = (uint64_t *)PHYS_TO_VIRT(vs->root);
	uint64_t *l4_new = (uint64_t *)PHYS_TO_VIRT(root_phys);

	uint64_t *kpml4 = (uint64_t *)PHYS_TO_VIRT(pml4);
	for (int i = 1; i < 512; i++)
		l4_new[i] = kpml4[i];

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
				l3_new[j] = l3_old[j];
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
						uint64_t pa = entry & ~0x1FFFFFUL;
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
					if (entry & PTE_U) {
						entry &= ~PTE_W;
						l1_old[l] = entry;
						pmm_hold((void *)(entry & ~0xFFFUL));
					}
					l1_new[l] = entry;
				}
			}
		}
	}

	spin_unlock_irq(&vs->fault_lock, lf);

	uint64_t cr3;
	__asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");

	return new_vs;

oom:
	vm_destroy(new_vs);
	return NULL;
}
