#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
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
	/* TODO: walk and free L2/L3 tables */
	buddy_free((uint64_t)vs->root, 0);
	buddy_free((uint64_t)vs, 0);
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

	l3[l3i] = 0;
	__asm__ volatile(
		"dsb ishst\n"
		"tlbi vaae1is, %0\n"
		"dsb ish\n"
		"isb\n"
		:: "r"(vaddr >> 12)
	);
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
