#ifndef VM_H
#define VM_H

#include <uapi/types.h>

#define VM_READ   (1 << 0)
#define VM_WRITE  (1 << 1)
#define VM_EXEC   (1 << 2)
#define VM_USER   (1 << 3)

typedef struct {
	uint64_t *root;   /* physical address of top-level page table */
} vm_space_t;

vm_space_t *vm_create(void);
void        vm_destroy(vm_space_t *vs);
int         vm_map(vm_space_t *vs, uint64_t vaddr, uint64_t paddr, uint32_t flags);
void        vm_unmap(vm_space_t *vs, uint64_t vaddr);
void        vm_activate(void);
void        vm_set_kernel_pt(uint64_t phys);

/* arch-specific: load page table into hardware */
void arch_vm_switch(uint64_t root_phys);

#endif
