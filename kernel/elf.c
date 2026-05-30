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

	/* Reject dynamically linked binaries — no dynamic linker support */
	for (uint16_t i = 0; i < hdr->phnum; i++) {
		if (hdr->phoff + (i + 1) * sizeof(elf64_phdr_t) > len) return -1;
		const elf64_phdr_t *ph =
			(const elf64_phdr_t *)(bytes + hdr->phoff + i * sizeof(elf64_phdr_t));
		if (ph->type == PT_INTERP) return -1;
	}

	extern uint64_t arch_vm_get_paddr(uint64_t *root, uint64_t vaddr);

	uint64_t load_end   = 0;
	uint64_t load_start = ~0UL;
	uint64_t load_bias  = 0;

	/* ET_DYN: pick a load base so the binary doesn't land at 0 */
	if (hdr->type == ET_DYN)
		load_bias = 0x400000UL;

	/* Pass 1: validate, and map every page in the loadable span exactly once,
	 * with the union of permissions of all segments that touch it. Adjacent
	 * segments (e.g. R-X text and RW data) commonly share a boundary page; a
	 * page must not be re-allocated per segment or the earlier segment's bytes
	 * in that page would be lost and the old page leaked. */
	for (uint16_t i = 0; i < hdr->phnum; i++) {
		const elf64_phdr_t *ph =
			(const elf64_phdr_t *)(bytes + hdr->phoff + i * sizeof(elf64_phdr_t));
		if (ph->type != PT_LOAD || ph->memsz == 0) continue;
		if (ph->offset + ph->filesz > len) return -1;

		uint64_t vbase  = load_bias + ph->vaddr;
		uint64_t vstart = vbase & ~(uint64_t)(PAGE_SIZE - 1);
		uint64_t vend   = (vbase + ph->memsz + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
		if (vend > load_end) load_end = vend;
		if (vstart < load_start) load_start = vstart;

		uint32_t vmflags = VM_USER | VM_READ;
		if (ph->flags & ELF_PF_W) vmflags |= VM_WRITE;
		if (ph->flags & ELF_PF_X) vmflags |= VM_EXEC;

		for (uint64_t va = vstart; va < vend; va += PAGE_SIZE) {
			uint64_t pa = arch_vm_get_paddr(vm->root, va);
			if (pa) {
				/* Shared boundary page: keep its data, widen permissions. */
				vm_map(vm, va, pa, vmflags | VM_READ);
				continue;
			}
			void *page_phys = buddy_alloc(0);
			if (!page_phys) return -1;
			memset((void *)PHYS_TO_VIRT(page_phys), 0, PAGE_SIZE);
			vm_map(vm, va, (uint64_t)page_phys, vmflags);
		}
	}

	/* Pass 2: copy file-backed bytes into the now-mapped pages. memsz>filesz
	 * (bss) stays zero from the page allocation above. Copy per physical page
	 * so a segment spanning multiple pages — or sharing a page with another —
	 * lands correctly regardless of which vm is currently active. */
	for (uint16_t i = 0; i < hdr->phnum; i++) {
		const elf64_phdr_t *ph =
			(const elf64_phdr_t *)(bytes + hdr->phoff + i * sizeof(elf64_phdr_t));
		if (ph->type != PT_LOAD || ph->filesz == 0) continue;

		uint64_t vbase = load_bias + ph->vaddr;
		for (uint64_t off = 0; off < ph->filesz; ) {
			uint64_t va       = vbase + off;
			uint64_t pa       = arch_vm_get_paddr(vm->root, va);
			if (!pa) return -1;
			uint64_t page_off = va & (PAGE_SIZE - 1);
			uint64_t n        = PAGE_SIZE - page_off;
			if (n > ph->filesz - off) n = ph->filesz - off;
			memcpy((uint8_t *)PHYS_TO_VIRT(pa) + page_off,
			       bytes + ph->offset + off, n);
			off += n;
		}
	}

#if defined(__aarch64__)
	/* Make freshly written instructions visible to every CPU's I-cache. The
	 * pages are recycled from the allocator and may hold another program's
	 * code in some core's I-cache; clean D to PoC, then invalidate I across
	 * the inner-shareable domain (ic ialluis, not the CPU-local ic iallu) or a
	 * core the process later migrates to fetches stale instructions and jumps
	 * into garbage. */
	for (uint64_t va = load_start & ~(uint64_t)(PAGE_SIZE - 1); va < load_end; va += PAGE_SIZE) {
		uint64_t pa = arch_vm_get_paddr(vm->root, va);
		if (!pa) continue;
		uintptr_t p = (uintptr_t)PHYS_TO_VIRT(pa);
		for (uintptr_t l = p; l < p + PAGE_SIZE; l += 64)
			__asm__ volatile("dc cvac, %0" :: "r"(l) : "memory");
	}
	__asm__ volatile("dsb ish; ic ialluis; dsb ish; isb" ::: "memory");
#endif

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
