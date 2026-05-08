#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>

#define PT_ENTRIES  512
#define PTE_P   (1UL << 0)
#define PTE_W   (1UL << 1)
#define PTE_U   (1UL << 2)
#define PTE_PWT (1UL << 3)
#define PTE_PCD (1UL << 4)
#define PTE_PS  (1UL << 7)

extern uint64_t pml4[];

static uint64_t *alloc_table(void)
{
	void *p = buddy_alloc(0);
	if (!p) kpanic("vm: out of memory");
	memset(p, 0, PAGE_SIZE);
	return p;
}

vm_space_t *vm_create(void)
{
	vm_space_t *vs = buddy_alloc(0);
	if (!vs) kpanic("vm: out of memory");

	vs->root = alloc_table();
	memcpy(vs->root, pml4, PT_ENTRIES * sizeof(uint64_t));

	if (vs->root[0] & PTE_P) {
		uint64_t *kpdpt = (uint64_t *)(vs->root[0] & ~0xFFFUL);
		uint64_t *pdpt  = alloc_table();
		memcpy(pdpt, kpdpt, PT_ENTRIES * sizeof(uint64_t));
		vs->root[0] = (uint64_t)pdpt | (vs->root[0] & 0xFFFUL);

		if (pdpt[0] & PTE_P) {
			uint64_t *kpd = (uint64_t *)(pdpt[0] & ~0xFFFUL);
			uint64_t *pd  = alloc_table();
			memcpy(pd, kpd, PT_ENTRIES * sizeof(uint64_t));
			pdpt[0] = (uint64_t)pd | (pdpt[0] & 0xFFFUL);
		}
	}

	vs->refcount = 1;
	return vs;
}

vm_space_t *vm_get(vm_space_t *vs)
{
	if (vs) vs->refcount++;
	return vs;
}

void vm_destroy(vm_space_t *vs)
{
	if (!vs) return;
	if (vs->refcount > 1) { vs->refcount--; return; }
	vs->refcount = 0;
	uint64_t *l4 = vs->root;

	/* We only walk index 0 (user space) */
	for (int i = 0; i < 1; i++) {
		if (!(l4[i] & PTE_P)) continue;

		uint64_t *l3 = (uint64_t *)PHYS_TO_VIRT(l4[i] & ~0xFFFUL);
		for (int j = 0; j < 512; j++) {
			if (!(l3[j] & PTE_P)) continue;
			if (l3[j] & PTE_PS) continue;

			uint64_t *l2 = (uint64_t *)PHYS_TO_VIRT(l3[j] & ~0xFFFUL);
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

				uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT(l2[k] & ~0xFFFUL);
				for (int l = 0; l < 512; l++) {
					if (!(l1[l] & PTE_P)) continue;
					if (l1[l] & PTE_U) {
						pmm_free((void *)(l1[l] & ~0xFFFUL));
					}
				}
				buddy_free(l2[k] & ~0xFFFUL, 0);
			}
			buddy_free(l3[j] & ~0xFFFUL, 0);
		}
		buddy_free(l4[i] & ~0xFFFUL, 0);
	}

	buddy_free((uint64_t)VIRT_TO_PHYS(l4), 0);
	buddy_free((uint64_t)VIRT_TO_PHYS(vs), 0);
}

static uint64_t *get_or_alloc(uint64_t *table, uint64_t idx, uint64_t eflags)
{
	if (table[idx] & PTE_P) {
		if (table[idx] & PTE_PS) {
			/* split 2MB huge page into 512 4KB pages */
			uint64_t base = table[idx] & ~0x1FFFFFUL;
			uint64_t attr = table[idx] & (PTE_P | PTE_W);
			uint64_t *pt  = alloc_table();
			for (int j = 0; j < PT_ENTRIES; j++)
				pt[j] = (base + (uint64_t)j * 0x1000) | attr;
			table[idx] = (uint64_t)pt | PTE_P | PTE_W | PTE_U;
			/* flush TLB — whole CR3 reload since we're changing a huge entry */
			__asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
			return pt;
		}
		/* propagate PTE_U flag to intermediate tables */
		table[idx] |= eflags & PTE_U;
		return (uint64_t *)(table[idx] & ~0xFFFUL);
	}
	uint64_t *child = alloc_table();
	table[idx] = (uint64_t)child | eflags;
	return child;
}

int vm_map(vm_space_t *vs, uint64_t vaddr, uint64_t paddr, uint32_t flags)
{
	uint64_t eflags = PTE_P | PTE_W | PTE_U;

	uint64_t *pdp = get_or_alloc(vs->root,  (vaddr >> 39) & 0x1FF, eflags);
	if (!pdp) return -1;
	uint64_t *pd  = get_or_alloc(pdp,       (vaddr >> 30) & 0x1FF, eflags);
	if (!pd)  return -1;
	uint64_t *pt  = get_or_alloc(pd,        (vaddr >> 21) & 0x1FF, eflags);
	if (!pt)  return -1;

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
	uint64_t *pdp = (vs->root[(vaddr >> 39) & 0x1FF] & PTE_P)
	              ? (uint64_t *)(vs->root[(vaddr >> 39) & 0x1FF] & ~0xFFFUL) : 0;
	if (!pdp) return;
	uint64_t *pd = (pdp[(vaddr >> 30) & 0x1FF] & PTE_P)
	             ? (uint64_t *)(pdp[(vaddr >> 30) & 0x1FF] & ~0xFFFUL) : 0;
	if (!pd) return;
	uint64_t *pt = (pd[(vaddr >> 21) & 0x1FF] & PTE_P)
	             ? (uint64_t *)(pd[(vaddr >> 21) & 0x1FF] & ~0xFFFUL) : 0;
	if (!pt) return;

	uint64_t pte = pt[(vaddr >> 12) & 0x1FF];
	pt[(vaddr >> 12) & 0x1FF] = 0;
	__asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
	if ((pte & PTE_P) && (pte & PTE_U))
		pmm_free((void *)(pte & ~0xFFFUL));
}

uint64_t arch_vm_get_paddr(uint64_t *pml4, uint64_t vaddr)
{
	uint64_t p4i = (vaddr >> 39) & 0x1FF;
	uint64_t p3i = (vaddr >> 30) & 0x1FF;
	uint64_t p2i = (vaddr >> 21) & 0x1FF;
	uint64_t p1i = (vaddr >> 12) & 0x1FF;

	if (!(pml4[p4i] & PTE_P)) return 0;
	uint64_t *pdp = (uint64_t *)(pml4[p4i] & ~0xFFFUL);

	if (!(pdp[p3i] & PTE_P)) return 0;
	if (pdp[p3i] & PTE_PS) return (pdp[p3i] & ~0x3FFFFFFFUL) | (vaddr & 0x3FFFFFFFUL);

	uint64_t *pd = (uint64_t *)(pdp[p3i] & ~0xFFFUL);
	if (!(pd[p2i] & PTE_P)) return 0;
	if (pd[p2i] & PTE_PS) return (pd[p2i] & ~0x1FFFFFUL) | (vaddr & 0x1FFFFFUL);

	uint64_t *pt = (uint64_t *)(pd[p2i] & ~0xFFFUL);
	if (!(pt[p1i] & PTE_P)) return 0;
	return (pt[p1i] & ~0xFFFUL);
}

void arch_vm_switch(uint64_t root_phys)
{
	__asm__ volatile("mov %0, %%cr3" :: "r"(root_phys) : "memory");
}

vm_space_t *vm_copy(vm_space_t *vs)
{
	vm_space_t *new_vs = buddy_alloc(0);
	if (!new_vs) kpanic("vm_copy: out of memory");
	new_vs->root = alloc_table();

	uint64_t *l4_old = vs->root;
	uint64_t *l4_new = new_vs->root;

	uint64_t eflags = PTE_P | PTE_W | PTE_U;

	/* Only walk user space (PML4 index 0) */
	for (int i = 0; i < 1; i++) {
		if (!(l4_old[i] & PTE_P)) continue;

		uint64_t *l3_old = (uint64_t *)(l4_old[i] & ~0xFFFUL);
		uint64_t *l3_new = alloc_table();
		l4_new[i] = (uint64_t)l3_new | eflags;

		for (int j = 0; j < 512; j++) {
			if (!(l3_old[j] & PTE_P)) continue;
			if (l3_old[j] & PTE_PS) {
				/* 1GB huge page - just copy for now (no COW support) */
				l3_new[j] = l3_old[j];
				continue;
			}

			uint64_t *l2_old = (uint64_t *)(l3_old[j] & ~0xFFFUL);
			uint64_t *l2_new = alloc_table();
			l3_new[j] = (uint64_t)l2_new | eflags;

			for (int k = 0; k < 512; k++) {
				if (!(l2_old[k] & PTE_P)) continue;
				if (l2_old[k] & PTE_PS) {
					/* 2MB huge page */
					uint64_t entry = l2_old[k];
					if (entry & PTE_U) {
						entry &= ~PTE_W; /* Mark RO */
						l2_old[k] = entry;
						uint64_t pa = entry & ~0x1FFFFFUL;
						for (int l = 0; l < 512; l++)
							pmm_hold((void *)(pa + (uint64_t)l * PAGE_SIZE));
					}
					l2_new[k] = entry;
					continue;
				}

				uint64_t *l1_old = (uint64_t *)(l2_old[k] & ~0xFFFUL);
				uint64_t *l1_new = alloc_table();
				l2_new[k] = (uint64_t)l1_new | eflags;

				for (int l = 0; l < 512; l++) {
					if (!(l1_old[l] & PTE_P)) continue;
					uint64_t entry = l1_old[l];
					if (entry & PTE_U) {
						entry &= ~PTE_W; /* Mark RO */
						l1_old[l] = entry;
						pmm_hold((void *)(entry & ~0xFFFUL));
					}
					l1_new[l] = entry;
				}
			}
		}
	}

	/* Reload CR3 to flush TLB on parent */
	uint64_t cr3;
	__asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");

	new_vs->refcount = 1;
	return new_vs;
}
