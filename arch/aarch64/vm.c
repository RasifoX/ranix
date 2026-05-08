#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>

#define PT_ENTRIES  512
#define L12_TABLE   (PTE_VALID | PTE_TABLE)
#define L3_PAGE     (PTE_VALID | (1UL << 1) | PTE_AF)

extern uint64_t kernel_l1[];

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
	memcpy(vs->root, kernel_l1, PT_ENTRIES * sizeof(uint64_t));

	/* give each process a private copy of every L2 table referenced
	 * from the kernel L1, so vm_map can safely split blocks without
	 * affecting the kernel or other processes */
	for (int i = 0; i < PT_ENTRIES; i++) {
		if ((vs->root[i] & 3) == 3) {
			uint64_t *orig = (uint64_t *)(vs->root[i] & ~0xFFFUL);
			uint64_t *priv = alloc_table();
			memcpy(priv, orig, PT_ENTRIES * sizeof(uint64_t));
			vs->root[i] = (uint64_t)priv | PTE_TABLE | PTE_VALID;
		}
	}

	return vs;
}

void vm_destroy(vm_space_t *vs)
{
	if (!vs) return;
	uint64_t *l1 = vs->root;

	/* We only walk index 0 (user space) to avoid touching shared kernel tables */
	for (int i = 0; i < 1; i++) {
		if (!(l1[i] & PTE_VALID)) continue;
		if ((l1[i] & 3) != 3) continue; /* Block mapping */

		uint64_t *l2 = (uint64_t *)PHYS_TO_VIRT(l1[i] & ~0xFFFUL);
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

			uint64_t *l3 = (uint64_t *)PHYS_TO_VIRT(l2[j] & ~0xFFFUL);
			for (int k = 0; k < 512; k++) {
				if (!(l3[k] & PTE_VALID)) continue;
				if (l3[k] & PTE_USER) {
					pmm_free((void *)(l3[k] & ~0xFFFUL));
				}
			}
			buddy_free(l2[j] & ~0xFFFUL, 0);
		}
		buddy_free(l1[i] & ~0xFFFUL, 0);
	}

	buddy_free((uint64_t)VIRT_TO_PHYS(l1), 0);
	buddy_free((uint64_t)VIRT_TO_PHYS(vs), 0);
}

int vm_map(vm_space_t *vs, uint64_t vaddr, uint64_t paddr, uint32_t flags)
{
	uint64_t *l1 = vs->root;
	uint64_t l1i = (vaddr >> 30) & 0x1FF;
	uint64_t l2i = (vaddr >> 21) & 0x1FF;
	uint64_t l3i = (vaddr >> 12) & 0x1FF;

	uint64_t *l2;
	if (l1[l1i] & PTE_VALID) {
		if ((l1[l1i] & 3) != 3) return -1;  /* block, not table */
		l2 = (uint64_t *)(l1[l1i] & ~0xFFFUL);
	} else {
		l2 = alloc_table();
		l1[l1i] = (uint64_t)l2 | L12_TABLE;
	}

	uint64_t *l3;
	if (l2[l2i] & PTE_VALID) {
		if ((l2[l2i] & 3) != 3) {
			/* 2MB L2 block — split into 512 4KB L3 pages */
			uint64_t blk  = l2[l2i];
			uint64_t base = blk & ~0x1FFFFFUL;
			uint64_t attr = (blk & 0xFFCUL) | PTE_VALID | (1UL << 1);
			l3 = alloc_table();
			for (int j = 0; j < PT_ENTRIES; j++)
				l3[j] = (base + (uint64_t)j * PAGE_SIZE) | attr;
			l2[l2i] = (uint64_t)l3 | L12_TABLE;
			uint64_t va2m = vaddr & ~0x1FFFFFUL;
			__asm__ volatile("dsb ishst; tlbi vaae1is, %0; dsb ish; isb"
			                 :: "r"(va2m >> 12) : "memory");
		}
		l3 = (uint64_t *)(l2[l2i] & ~0xFFFUL);
	} else {
		l3 = alloc_table();
		l2[l2i] = (uint64_t)l3 | L12_TABLE;
	}

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
	uint64_t *l1 = vs->root;
	uint64_t l1i = (vaddr >> 30) & 0x1FF;
	uint64_t l2i = (vaddr >> 21) & 0x1FF;
	uint64_t l3i = (vaddr >> 12) & 0x1FF;

	if (!(l1[l1i] & PTE_VALID)) return;
	uint64_t *l2 = (uint64_t *)(l1[l1i] & ~0xFFFUL);

	if (!(l2[l2i] & PTE_VALID)) return;
	uint64_t *l3 = (uint64_t *)(l2[l2i] & ~0xFFFUL);

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

uint64_t arch_vm_get_paddr(uint64_t *l1, uint64_t vaddr)
{
	uint64_t l1i = (vaddr >> 30) & 0x1FF;
	uint64_t l2i = (vaddr >> 21) & 0x1FF;
	uint64_t l3i = (vaddr >> 12) & 0x1FF;

	if (!(l1[l1i] & PTE_VALID)) return 0;
	if ((l1[l1i] & 3) != 3) return (l1[l1i] & ~0x3FFFFFFFUL) | (vaddr & 0x3FFFFFFFUL);

	uint64_t *l2 = (uint64_t *)(l1[l1i] & ~0xFFFUL);
	if (!(l2[l2i] & PTE_VALID)) return 0;
	if ((l2[l2i] & 3) != 3) return (l2[l2i] & ~0x1FFFFFUL) | (vaddr & 0x1FFFFFUL);

	uint64_t *l3 = (uint64_t *)(l2[l2i] & ~0xFFFUL);
	if (!(l3[l3i] & PTE_VALID)) return 0;
	return (l3[l3i] & ~0xFFFUL);
}

void arch_vm_switch(uint64_t root_phys)
{
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
	vm_space_t *new_vs = buddy_alloc(0);
	if (!new_vs) kpanic("vm_copy: out of memory");
	new_vs->root = alloc_table();

	uint64_t *l1_old = vs->root;
	uint64_t *l1_new = new_vs->root;

	/* We only copy the first 1GB (index 0) which contains user space.
	 * Kernel-only spaces or upper 1GB blocks are managed differently. */
	for (int i = 0; i < 1; i++) {
		if (!(l1_old[i] & PTE_VALID)) continue;
		if ((l1_old[i] & 3) != 3) {
			/* Block mapping (1GB) — Not supported for user COW yet, 
			 * just copy as is. */
			l1_new[i] = l1_old[i];
			continue;
		}

		uint64_t *l2_old = (uint64_t *)(l1_old[i] & ~0xFFFUL);
		uint64_t *l2_new = alloc_table();
		l1_new[i] = (uint64_t)l2_new | L12_TABLE;

		for (int j = 0; j < 512; j++) {
			if (!(l2_old[j] & PTE_VALID)) continue;
			if ((l2_old[j] & 3) != 3) {
				/* 2MB Block mapping */
				uint64_t entry = l2_old[j];
				if (entry & PTE_USER) {
					entry |= PTE_RO;
					l2_old[j] = entry;
					/* Mark all 512 pages in this 2MB block as held.
					 * Since our pmm_hold works on 4KB, we loop. */
					uint64_t pa = entry & ~0x1FFFFFUL;
					for (int k = 0; k < 512; k++)
						pmm_hold((void *)(pa + (uint64_t)k * PAGE_SIZE));
				}
				l2_new[j] = entry;
				continue;
			}

			uint64_t *l3_old = (uint64_t *)(l2_old[j] & ~0xFFFUL);
			uint64_t *l3_new = alloc_table();
			l2_new[j] = (uint64_t)l3_new | L12_TABLE;

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

	/* Flush TLB to ensure RO takes effect on the parent too */
	__asm__ volatile("dsb ishst; tlbi vmalle1is; dsb ish; isb" ::: "memory");

	return new_vs;
}
