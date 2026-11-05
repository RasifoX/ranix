#include <kernel/elf.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/mm/heap.h>
#include <kernel/vfs.h>
#include <kernel/cgroup.h>
#include <kernel/proc.h>
#include <kernel/hal.h>

/* Image source: a buffer already in memory, or an open file. Reading through
 * this keeps a large executable out of one contiguous kernel allocation. */
typedef struct {
	const uint8_t *mem;
	int            fd;
	size_t         len;
} elf_src_t;

static int src_read(const elf_src_t *s, uint64_t off, void *dst, size_t n)
{
	if (off > s->len || n > s->len - off) return -1;
	if (s->mem) { memcpy(dst, s->mem + off, n); return 0; }
	if (vfs_seek(s->fd, (off_t)off, 0) != (off_t)off) return -1;
	uint8_t *d = dst;
	while (n) {
		ssize_t r = vfs_read(s->fd, d, n);
		if (r <= 0) return -1;
		d += r; n -= (size_t)r;
	}
	return 0;
}

static int elf_load_src(vm_space_t *vm, const elf_src_t *src,
                        uint64_t load_bias_hint, elf_info_t *out)
{
	elf64_hdr_t hdr_buf;
	if (src_read(src, 0, &hdr_buf, sizeof(hdr_buf)) != 0) return -1;
	const elf64_hdr_t *hdr = &hdr_buf;

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

	process_t *cur = task_current();
	int err = -1;
	size_t ph_bytes = (size_t)hdr->phnum * sizeof(elf64_phdr_t);
	elf64_phdr_t *phdrs = kmalloc(ph_bytes);
	if (!phdrs) return -1;
	if (src_read(src, hdr->phoff, phdrs, ph_bytes) != 0) goto fail;

	/* Record the interpreter path (if any); the caller loads it separately
	 * and the in-userspace dynamic linker handles relocation. */
	out->interp[0] = '\0';
	for (uint16_t i = 0; i < hdr->phnum; i++) {
		const elf64_phdr_t *ph = &phdrs[i];
		if (ph->type == PT_INTERP && ph->filesz > 0) {
			size_t n = ph->filesz;
			if (n > sizeof(out->interp) - 1) n = sizeof(out->interp) - 1;
			if (src_read(src, ph->offset, out->interp, n) != 0) goto fail;
			out->interp[n] = '\0';
		}
	}

	extern uint64_t arch_vm_get_paddr(uint64_t *root, uint64_t vaddr);

	uint64_t load_end   = 0;
	uint64_t load_start = ~0UL;
	uint64_t load_bias  = 0;

	/* ET_DYN: place at the caller's hint, or a default so it doesn't land at 0 */
	if (hdr->type == ET_DYN) {
		/* An ET_DYN with no interpreter may itself be a loader run directly
		 * (`ld.so prog`). It must not sit where the ET_EXEC it maps goes, or
		 * mapping that program replaces the loader's own running code. Wine's
		 * preloader reserves up to 0x68000000 with MAP_FIXED, so stay above it. */
		uint64_t dflt = out->interp[0] ? 0x400000UL : 0x100000000UL;
		load_bias = load_bias_hint ? load_bias_hint : dflt;
	}

	/* Pass 1: validate, and map every page in the loadable span exactly once,
	 * with the union of permissions of all segments that touch it. Adjacent
	 * segments (e.g. R-X text and RW data) commonly share a boundary page; a
	 * page must not be re-allocated per segment or the earlier segment's bytes
	 * in that page would be lost and the old page leaked. */
	for (uint16_t i = 0; i < hdr->phnum; i++) {
		const elf64_phdr_t *ph = &phdrs[i];
		if (ph->type != PT_LOAD || ph->memsz == 0) continue;
		if (ph->offset > src->len || ph->filesz > src->len - ph->offset) goto fail;

		uint64_t vbase  = load_bias + ph->vaddr;
		uint64_t vstart = vbase & ~(uint64_t)(PAGE_SIZE - 1);
		uint64_t vend   = (vbase + ph->memsz + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
		/* A crafted header must not name the kernel half: page tables above the
		 * user slot are shared by every address space. */
		if (ph->vaddr > vbase || vend <= vstart || !access_ok((void *)vstart, vend - vstart))
			goto fail;
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
			if (cgroup_charge_mem(vm, cur ? cur->cgroup : NULL, PAGE_SIZE) != 0) { err = -12; goto fail; }
			void *page_phys = buddy_alloc(0);
			if (!page_phys) goto fail;
			memset((void *)PHYS_TO_VIRT(page_phys), 0, PAGE_SIZE);
			vm_map(vm, va, (uint64_t)page_phys, vmflags);
		}
	}

	/* Pass 2: copy file-backed bytes into the now-mapped pages. memsz>filesz
	 * (bss) stays zero from the page allocation above. Copy per physical page
	 * so a segment spanning multiple pages — or sharing a page with another —
	 * lands correctly regardless of which vm is currently active. */
	for (uint16_t i = 0; i < hdr->phnum; i++) {
		const elf64_phdr_t *ph = &phdrs[i];
		if (ph->type != PT_LOAD || ph->filesz == 0) continue;

		uint64_t vbase = load_bias + ph->vaddr;
		for (uint64_t off = 0; off < ph->filesz; ) {
			uint64_t va       = vbase + off;
			uint64_t pa       = arch_vm_get_paddr(vm->root, va);
			if (!pa) goto fail;
			uint64_t page_off = va & (PAGE_SIZE - 1);
			uint64_t n        = PAGE_SIZE - page_off;
			if (n > ph->filesz - off) n = ph->filesz - off;
			if (src_read(src, ph->offset + off,
			             (uint8_t *)PHYS_TO_VIRT(pa) + page_off, n) != 0)
				goto fail;
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
		const elf64_phdr_t *ph = &phdrs[i];
		if (ph->type != PT_LOAD) continue;
		if (hdr->phoff >= ph->offset && hdr->phoff < ph->offset + ph->filesz) {
			phdr_va = load_bias + ph->vaddr + (hdr->phoff - ph->offset);
			break;
		}
	}

	if (!access_ok((void *)(load_bias + hdr->entry), 1)) goto fail;
	out->entry     = load_bias + hdr->entry;
	out->load_end  = load_end;
	out->load_bias = load_bias;
	out->phdr_va   = phdr_va;
	out->phnum     = hdr->phnum;
	out->phentsize = hdr->phentsize ? hdr->phentsize : sizeof(elf64_phdr_t);
	kfree(phdrs);
	return 0;

fail:
	kfree(phdrs);
	return err;
}

int elf_load(vm_space_t *vm, const void *data, size_t len,
             uint64_t load_bias_hint, elf_info_t *out)
{
	elf_src_t src = { .mem = data, .fd = -1, .len = len };
	return elf_load_src(vm, &src, load_bias_hint, out);
}

int elf_load_fd(vm_space_t *vm, int fd, size_t len,
                uint64_t load_bias_hint, elf_info_t *out)
{
	elf_src_t src = { .mem = NULL, .fd = fd, .len = len };
	return elf_load_src(vm, &src, load_bias_hint, out);
}
