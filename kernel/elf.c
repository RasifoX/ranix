#include <kernel/elf.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>

int elf_load(vm_space_t *vm, const void *data, size_t len, elf_info_t *out)
{
	if (len < sizeof(elf64_hdr_t)) return -1;

	const elf64_hdr_t *hdr = data;

	if (*(uint32_t *)hdr->magic != ELF_MAGIC) return -1;
	if (hdr->class   != ELF_CLASS64)           return -1;
	if (hdr->data    != ELF_LSB)               return -1;
	if (hdr->type != ET_EXEC && hdr->type != ET_DYN) return -1;

#if defined(__aarch64__)
	if (hdr->machine != EM_AARCH64) return -1;
#elif defined(__x86_64__)
	if (hdr->machine != EM_X86_64)  return -1;
#endif

	if (hdr->phoff == 0 || hdr->phnum == 0) return -1;

	const uint8_t *bytes = data;
	uint64_t load_end  = 0;
	uint64_t load_bias = 0;

	/* ET_DYN: pick a load base so the binary doesn't land at 0 */
	if (hdr->type == ET_DYN)
		load_bias = 0x400000UL;

	for (uint16_t i = 0; i < hdr->phnum; i++) {
		if (hdr->phoff + (i + 1) * sizeof(elf64_phdr_t) > len) return -1;

		const elf64_phdr_t *ph =
			(const elf64_phdr_t *)(bytes + hdr->phoff + i * sizeof(elf64_phdr_t));

		if (ph->type != PT_LOAD || ph->memsz == 0) continue;

		uint64_t vbase  = load_bias + ph->vaddr;
		uint64_t vstart = vbase & ~(uint64_t)(PAGE_SIZE - 1);
		uint64_t vend   = (vbase + ph->memsz + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

		if (vend > load_end) load_end = vend;
		if (ph->offset + ph->filesz > len) return -1;

		uint32_t vmflags = VM_USER | VM_READ;
		if (ph->flags & ELF_PF_W) vmflags |= VM_WRITE;
		if (ph->flags & ELF_PF_X) vmflags |= VM_EXEC;

		for (uint64_t va = vstart; va < vend; va += PAGE_SIZE) {
			void *page = buddy_alloc(0);
			if (!page) return -1;
			memset(page, 0, PAGE_SIZE);

			if (ph->filesz > 0) {
				uint64_t copy_va_start = (va < vbase) ? vbase : va;
				uint64_t copy_va_end   = va + PAGE_SIZE;
				if (copy_va_end > vbase + ph->filesz)
					copy_va_end = vbase + ph->filesz;

				if (copy_va_start < copy_va_end) {
					uint64_t dst_off = copy_va_start - va;
					uint64_t src_off = copy_va_start - vbase;
					uint64_t cplen   = copy_va_end - copy_va_start;
					memcpy((uint8_t *)page + dst_off,
					       bytes + ph->offset + src_off,
					       cplen);
				}
			}

			vm_map(vm, va, (uint64_t)page, vmflags);

#if defined(__aarch64__)
			{
				uintptr_t p = (uintptr_t)page;
				for (uintptr_t l = p; l < p + PAGE_SIZE; l += 64)
					__asm__ volatile("dc civac, %0" :: "r"(l) : "memory");
				__asm__ volatile("dsb ish; ic iallu; dsb ish; isb" ::: "memory");
			}
#endif
		}
	}

	/* Compute virtual address of program header table */
	uint64_t phdr_va = 0;
	for (uint16_t i = 0; i < hdr->phnum; i++) {
		const elf64_phdr_t *ph =
			(const elf64_phdr_t *)(bytes + hdr->phoff + i * sizeof(elf64_phdr_t));
		if (ph->type != PT_LOAD) continue;
		if (hdr->phoff >= ph->offset && hdr->phoff < ph->offset + ph->filesz) {
			phdr_va = load_bias + ph->vaddr + (hdr->phoff - ph->offset);
			break;
		}
	}

	out->entry     = load_bias + hdr->entry;
	out->load_end  = load_end;
	out->load_bias = load_bias;
	out->phdr_va   = phdr_va;
	out->phnum     = hdr->phnum;
	out->phentsize = hdr->phentsize ? hdr->phentsize : sizeof(elf64_phdr_t);
	return 0;
}
