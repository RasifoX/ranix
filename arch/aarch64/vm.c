#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <asm/mmu.h>

#define PT_ENTRIES  512
#define L12_TABLE   (PTE_VALID | PTE_TABLE)
#define L3_PAGE     (PTE_VALID | (1UL << 1) | PTE_AF)

extern uint64_t kernel_l1[];

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

	uint64_t root_phys = alloc_table();
	if (!root_phys) { buddy_free(VIRT_TO_PHYS(vs), 0); return NULL; }
	vs->root = (uint64_t *)root_phys;

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

	if (!vs->root) { buddy_free(VIRT_TO_PHYS(vs), 0); return; }

	uint64_t l1_phys = (uint64_t)vs->root;
	uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT(l1_phys);

	for (int i = 0; i < 2; i++) {
		if (!(l1[i] & PTE_VALID)) continue;
		if ((l1[i] & 3) != 3) continue; /* Block mapping */

		uint64_t l2_phys = l1[i] & ~0xFFFUL;
		uint64_t *l2 = (uint64_t *)PHYS_TO_VIRT(l2_phys);
		for (int j = 0; j < 512; j++) {
			if (!(l2[j] & PTE_VALID)) continue;
			if ((l2[j] & 3) != 3) {
				/* 2MB block */
				if (l2[j] & PTE_USER) {
					uint64_t pa = l2[j] & ~0x1FFFFFUL;
					for (int k = 0; k < 512; k++)
						pmm_free((void *)(pa + (uint64_t)k * PAGE_SIZE));
				}
				continue;
			}

			uint64_t l3_phys = l2[j] & ~0xFFFUL;
			uint64_t *l3 = (uint64_t *)PHYS_TO_VIRT(l3_phys);
			int is_user_l3 = 0;
			for (int k = 0; k < 512; k++) {
				if (!(l3[k] & PTE_VALID)) continue;
				if (l3[k] & PTE_USER) {
					is_user_l3 = 1;
					pmm_free((void *)(l3[k] & ~0xFFFUL));
				}
			}
			/* Don't free kernel-inherited L3 tables (e.g. framebuffer split).
			 * They are shared via the private L2 copy and must not be recycled. */
			if (is_user_l3) buddy_free(l3_phys, 0);
		}
		buddy_free(l2_phys, 0);
	}

	buddy_free(l1_phys, 0);
	buddy_free(VIRT_TO_PHYS(vs), 0);
}

int vm_map(vm_space_t *vs, uint64_t vaddr, uint64_t paddr, uint32_t flags)
{
	uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT(vs->root);
	uint64_t l1i = (vaddr >> 30) & 0x1FF;
	uint64_t l2i = (vaddr >> 21) & 0x1FF;
	uint64_t l3i = (vaddr >> 12) & 0x1FF;

	uint64_t l2_phys;
	if (l1[l1i] & PTE_VALID) {
		if ((l1[l1i] & 3) != 3) return -1;
		l2_phys = l1[l1i] & ~0xFFFUL;
	} else {
		l2_phys = alloc_table();
		if (!l2_phys) return -1;
		l1[l1i] = l2_phys | L12_TABLE;
	}

	uint64_t *l2 = (uint64_t *)PHYS_TO_VIRT(l2_phys);
	uint64_t l3_phys;
	if (l2[l2i] & PTE_VALID) {
		if ((l2[l2i] & 3) != 3) {
			/* 2MB L2 block — split into 512 4KB L3 pages */
			uint64_t blk  = l2[l2i];
			uint64_t base = blk & ~0x1FFFFFUL;
			uint64_t attr = (blk & 0xFFCUL) | PTE_VALID | (1UL << 1);
			l3_phys = alloc_table();
			if (!l3_phys) return -1;
			uint64_t *l3_virt = (uint64_t *)PHYS_TO_VIRT(l3_phys);
			for (int j = 0; j < PT_ENTRIES; j++)
				l3_virt[j] = (base + (uint64_t)j * PAGE_SIZE) | attr;
			l2[l2i] = l3_phys | L12_TABLE;
			uint64_t va2m = vaddr & ~0x1FFFFFUL;
			__asm__ volatile("dsb ishst; tlbi vaae1is, %0; dsb ish; isb"
			                 :: "r"(va2m >> 12) : "memory");
		} else {
			l3_phys = l2[l2i] & ~0xFFFUL;
		}
	} else {
		l3_phys = alloc_table();
		if (!l3_phys) return -1;
		l2[l2i] = l3_phys | L12_TABLE;
	}

	uint64_t *l3 = (uint64_t *)PHYS_TO_VIRT(l3_phys);

	uint64_t attr = PTE_ATTR_NORMAL;
	if (flags & VM_NOCACHE)     attr = PTE_ATTR_NORMAL_NC;
	if (flags & VM_DEVICE)      attr = PTE_ATTR_DEVICE;
	if (flags & VM_USER)        attr |= PTE_USER;
	if (!(flags & VM_WRITE))    attr |= PTE_RO;

	l3[l3i] = (paddr & ~0xFFFUL) | L3_PAGE | attr;

	__asm__ volatile(
		"dsb ishst\n"
		"tlbi vaae1is, %0\n"
		"dsb ish\n"
		"isb\n"
		:: "r"(vaddr >> 12)
	);
	return 0;
}

void vm_unmap(vm_space_t *vs, uint64_t vaddr)
{
	uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT(vs->root);
	uint64_t l1i = (vaddr >> 30) & 0x1FF;
	uint64_t l2i = (vaddr >> 21) & 0x1FF;
	uint64_t l3i = (vaddr >> 12) & 0x1FF;

	if (!(l1[l1i] & PTE_VALID)) return;
	uint64_t *l2 = (uint64_t *)PHYS_TO_VIRT(l1[l1i] & ~0xFFFUL);

	if (!(l2[l2i] & PTE_VALID)) return;
	uint64_t *l3 = (uint64_t *)PHYS_TO_VIRT(l2[l2i] & ~0xFFFUL);

	uint64_t pte = l3[l3i];
	l3[l3i] = 0;
	__asm__ volatile(
		"dsb ishst\n"
		"tlbi vaae1is, %0\n"
		"dsb ish\n"
		"isb\n"
		:: "r"(vaddr >> 12)
	);
	/* Free the physical page if it was a user allocation */
	if ((pte & PTE_VALID) && (pte & PTE_USER))
		pmm_free((void *)(pte & ~0xFFFUL));
}

uint64_t arch_vm_get_paddr(uint64_t *l1_phys, uint64_t vaddr)
{
	uint64_t *l1 = (uint64_t *)PHYS_TO_VIRT(l1_phys);
	uint64_t l1i = (vaddr >> 30) & 0x1FF;
	uint64_t l2i = (vaddr >> 21) & 0x1FF;
	uint64_t l3i = (vaddr >> 12) & 0x1FF;

	if (!(l1[l1i] & PTE_VALID)) return 0;
	if ((l1[l1i] & 3) != 3) return (l1[l1i] & ~0x3FFFFFFFUL) | (vaddr & 0x3FFFFFFFUL);

	uint64_t *l2 = (uint64_t *)PHYS_TO_VIRT(l1[l1i] & ~0xFFFUL);
	if (!(l2[l2i] & PTE_VALID)) return 0;
	if ((l2[l2i] & 3) != 3) return (l2[l2i] & ~0x1FFFFFUL) | (vaddr & 0x1FFFFFUL);

	uint64_t *l3 = (uint64_t *)PHYS_TO_VIRT(l2[l2i] & ~0xFFFUL);
	if (!(l3[l3i] & PTE_VALID)) return 0;
	return (l3[l3i] & ~0xFFFUL);
}

void arch_vm_switch(uint64_t root_phys)
{
	/* Skip the reload + full TLB flush when the address space is unchanged
	 * (thread switches, re-running the same task, busy yields). Without
	 * ASIDs the flush is mandatory on a real AS change, but identical here.
	 * Safe against root reuse: task_exit switches to the kernel PT before
	 * vm_destroy frees a root, and shared VMs are refcounted, so TTBR0 never
	 * holds a freed-and-reused root. Big win under TCG emulation. */
	uint64_t cur;
	__asm__ volatile("mrs %0, ttbr0_el1" : "=r"(cur));
	if (cur == root_phys)
		return;
	__asm__ volatile(
		"msr ttbr0_el1, %0\n"
		"isb\n"
		"tlbi vmalle1is\n"
		"dsb ish\n"
		"isb\n"
		:: "r"(root_phys)
	);
}

vm_space_t *vm_copy(vm_space_t *vs)
{
	void *new_vs_phys = buddy_alloc(0);
	if (!new_vs_phys) return NULL;
	vm_space_t *new_vs = (vm_space_t *)PHYS_TO_VIRT(new_vs_phys);
	new_vs->root = NULL;
	new_vs->refcount = 1;

	uint64_t root_phys = alloc_table();
	if (!root_phys) goto oom;
	new_vs->root = (uint64_t *)root_phys;

	uint64_t *l1_old = (uint64_t *)PHYS_TO_VIRT(vs->root);
	uint64_t *l1_new = (uint64_t *)PHYS_TO_VIRT(root_phys);

	for (int i = 2; i < PT_ENTRIES; i++)
		l1_new[i] = l1_old[i];

	for (int i = 0; i < 2; i++) {
		if (!(l1_old[i] & PTE_VALID)) continue;
		if ((l1_old[i] & 3) != 3) {
			l1_new[i] = l1_old[i];
			continue;
		}

		uint64_t *l2_old = (uint64_t *)PHYS_TO_VIRT(l1_old[i] & ~0xFFFUL);
		uint64_t l2_new_phys = alloc_table();
		if (!l2_new_phys) goto oom;
		uint64_t *l2_new = (uint64_t *)PHYS_TO_VIRT(l2_new_phys);
		l1_new[i] = l2_new_phys | L12_TABLE;

		for (int j = 0; j < 512; j++) {
			if (!(l2_old[j] & PTE_VALID)) continue;
			if ((l2_old[j] & 3) != 3) {
				uint64_t entry = l2_old[j];
				if (entry & PTE_USER) {
					entry |= PTE_RO;
					l2_old[j] = entry;
					uint64_t pa = entry & ~0x1FFFFFUL;
					for (int k = 0; k < 512; k++)
						pmm_hold((void *)(pa + (uint64_t)k * PAGE_SIZE));
				}
				l2_new[j] = entry;
				continue;
			}

			uint64_t *l3_old = (uint64_t *)PHYS_TO_VIRT(l2_old[j] & ~0xFFFUL);
			uint64_t l3_new_phys = alloc_table();
			if (!l3_new_phys) goto oom;
			uint64_t *l3_new = (uint64_t *)PHYS_TO_VIRT(l3_new_phys);
			l2_new[j] = l3_new_phys | L12_TABLE;

			for (int k = 0; k < 512; k++) {
				if (!(l3_old[k] & PTE_VALID)) continue;
				uint64_t entry = l3_old[k];
				if (entry & PTE_USER) {
					entry |= PTE_RO;
					l3_old[k] = entry;
					pmm_hold((void *)(entry & ~0xFFFUL));
				}
				l3_new[k] = entry;
			}
		}
	}

	__asm__ volatile("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory");

	return new_vs;

oom:
	vm_destroy(new_vs);
	return NULL;
}
