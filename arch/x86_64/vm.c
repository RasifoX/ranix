#include <kernel/mm/vm.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>

#define PT_ENTRIES  512
#define PTE_P   (1UL << 0)
#define PTE_W   (1UL << 1)
#define PTE_U   (1UL << 2)
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
	return vs;
}

void vm_destroy(vm_space_t *vs)
{
	if (!vs) return;
	/* TODO: walk and free PDP/PD/PT tables */
	buddy_free((uint64_t)vs->root, 0);
	buddy_free((uint64_t)vs, 0);
}

static uint64_t *get_or_alloc(uint64_t *table, uint64_t idx, uint64_t eflags)
{
	if (table[idx] & PTE_P) {
		if (table[idx] & PTE_PS) return 0;  /* huge page */
		/* propagate flags (e.g. PTE_U) that the existing entry may lack */
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
	if (flags & VM_WRITE) entry |= PTE_W;
	if (flags & VM_USER)  entry |= PTE_U;
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

	pt[(vaddr >> 12) & 0x1FF] = 0;
	__asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
}

void arch_vm_switch(uint64_t root_phys)
{
	__asm__ volatile("mov %0, %%cr3" :: "r"(root_phys) : "memory");
}
