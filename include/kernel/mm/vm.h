#ifndef VM_H
#define VM_H

#include <uapi/types.h>
#include <kernel/sync.h>

#define VM_READ   (1 << 0)
#define VM_WRITE  (1 << 1)
#define VM_EXEC   (1 << 2)
#define VM_USER   (1 << 3)
#define VM_NOCACHE (1 << 4)
#define VM_DEVICE  (1 << 5)
#define VM_NOFREE  (1 << 6)   /* kernel-shared page (vDSO/vvar): unmap, never free on teardown */

#define HUGE_PAGE_SIZE  (2UL * 1024 * 1024)
#define HUGE_PAGE_MASK  (~(HUGE_PAGE_SIZE - 1))

/* 1 GB hugepage mmaps live in their own high VA window, clear of the normal
 * mmap region and stack (both under 2 GB), so a 1 GB-aligned block never
 * collides with them. */
#define HUGE1G_VA_BASE  0x100000000UL   /* 4 GB */

#define VM_SPACE_MAGIC 0x564D5350u   /* "VMSP" — set at create, checked at lifecycle ops */

typedef struct {
	uint64_t  *root;       /* physical address of top-level page table */
	uint32_t   refcount;   /* number of processes/threads sharing this space */
	uint32_t   asid;       /* aarch64 TLB tag (1..255; 0 = flush-on-switch); unused on x86 */
	spinlock_t fault_lock; /* serializes fault resolution among shared threads */
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
void        vm_activate(void);
void        vm_set_kernel_pt(uint64_t phys);

void        vm_init_kernel(uint64_t root_phys);
void       *ioremap(uint64_t phys_addr, size_t size, uint32_t flags);
int         vm_handle_fault(uint64_t vaddr, int is_write);

/* arch-specific: load page table into hardware. asid is the address space's
 * TLB tag on aarch64 (0 forces a flush); ignored on x86_64. */
void arch_vm_switch(uint64_t root_phys, uint32_t asid);

#endif
