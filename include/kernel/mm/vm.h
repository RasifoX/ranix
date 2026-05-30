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

typedef struct {
	uint64_t  *root;       /* physical address of top-level page table */
	uint32_t   refcount;   /* number of processes/threads sharing this space */
	spinlock_t fault_lock; /* serializes fault resolution among shared threads */
} vm_space_t;

extern vm_space_t *kernel_vm_space;

vm_space_t *vm_create(void);
vm_space_t *vm_copy(vm_space_t *vs);
vm_space_t *vm_get(vm_space_t *vs);   /* increment refcount */
void        vm_destroy(vm_space_t *vs); /* decrement refcount; free when 0 */
int         vm_map(vm_space_t *vs, uint64_t vaddr, uint64_t paddr, uint32_t flags);
void        vm_unmap(vm_space_t *vs, uint64_t vaddr);
void        vm_activate(void);
void        vm_set_kernel_pt(uint64_t phys);

void        vm_init_kernel(uint64_t root_phys);
void       *ioremap(uint64_t phys_addr, size_t size, uint32_t flags);
int         vm_handle_fault(uint64_t vaddr, int is_write);

/* arch-specific: load page table into hardware */
void arch_vm_switch(uint64_t root_phys);

#endif
