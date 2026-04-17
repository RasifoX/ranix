#include <kernel/elf.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>

int elf_load(vm_space_t *vm, const void *data, size_t len, uint64_t *entry_out)
{
	if (len < sizeof(elf64_hdr_t)) return -1;

	const elf64_hdr_t *hdr = data;

	/* validate */
	if (*(uint32_t *)hdr->magic != ELF_MAGIC) return -1;
	if (hdr->class   != ELF_CLASS64)           return -1;
	if (hdr->data    != ELF_LSB)               return -1;
	if (hdr->type    != ET_EXEC)               return -1;

#if defined(__aarch64__)
	if (hdr->machine != EM_AARCH64) return -1;
#elif defined(__x86_64__)
	if (hdr->machine != EM_X86_64)  return -1;
#endif

	if (hdr->phoff == 0 || hdr->phnum == 0) return -1;

	const uint8_t *bytes = data;

	for (uint16_t i = 0; i < hdr->phnum; i++) {
		if (hdr->phoff + (i + 1) * sizeof(elf64_phdr_t) > len) return -1;

		const elf64_phdr_t *ph =
			(const elf64_phdr_t *)(bytes + hdr->phoff + i * sizeof(elf64_phdr_t));

		if (ph->type != PT_LOAD || ph->memsz == 0) continue;

		if (ph->offset + ph->filesz > len) return -1;

		/* map pages for this segment */
		uint64_t vstart = ph->vaddr & ~(uint64_t)(PAGE_SIZE - 1);
		uint64_t vend   = (ph->vaddr + ph->memsz + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

		uint32_t vmflags = VM_USER | VM_READ;
		if (ph->flags & ELF_PF_W) vmflags |= VM_WRITE;
		if (ph->flags & ELF_PF_X) vmflags |= VM_EXEC;

		for (uint64_t va = vstart; va < vend; va += PAGE_SIZE) {
			void *page = buddy_alloc(0);
			if (!page) return -1;
			memset(page, 0, PAGE_SIZE);

			/* copy file data into this page */
			uint64_t page_off   = va - vstart;
			uint64_t seg_off    = (va > ph->vaddr) ? va - ph->vaddr : 0;
			uint64_t data_start = ph->vaddr - vstart;

			if (ph->filesz > 0) {
				uint64_t copy_va_start = (va < ph->vaddr) ? ph->vaddr : va;
				uint64_t copy_va_end   = va + PAGE_SIZE;
				if (copy_va_end > ph->vaddr + ph->filesz)
					copy_va_end = ph->vaddr + ph->filesz;

				if (copy_va_start < copy_va_end) {
					uint64_t dst_off = copy_va_start - va;
					uint64_t src_off = copy_va_start - ph->vaddr;
					uint64_t cplen   = copy_va_end - copy_va_start;
					memcpy((uint8_t *)page + dst_off,
					       bytes + ph->offset + src_off,
					       cplen);
				}
			}
			(void)page_off; (void)seg_off; (void)data_start;

			vm_map(vm, va, (uint64_t)page, vmflags);
		}
	}

	*entry_out = hdr->entry;
	return 0;
}
