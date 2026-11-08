// SPDX-License-Identifier: MIT
/*
 * Ranix: Gen8+ 48-bit (4-level) PPGTT for client address spaces.
 *
 * Copyright © 2026 Ranix. Entry format and PAT choice follow Linux i915
 * gt/gen8_ppgtt.c and gt/intel_gtt.h (MIT, Copyright © 2020 Intel Corporation):
 * PDEs are present|rw with PPAT index 0 (WB LLC), PTEs present|rw|PAT (index 4,
 * WB LLC/eLLC) as i915 uses for normal objects.
 *
 * Unused entries are never 0: as in gen8_ppgtt.c, each level's empty slots point
 * at a shared scratch table (scratch PT -> scratch page), so an access outside
 * every binding reads the zeroed scratch page instead of walking whatever sits
 * at physical address 0. Mapped pages are checked against the RAM map.
 */
#include <kernel/mm/pmm.h>
#include <kernel/mm/buddy.h>
#include <kernel/lib/string.h>
#include "../drm_priv.h"
#include "intel_display.h"
#include "ranix_vm.h"

#define PTE_PRESENT (1ULL << 0)
#define PTE_RW      (1ULL << 1)
#define PTE_PAT     (1ULL << 7)          /* _PAGE_PAT: PPAT index 4 */
#define ADDR_MASK   0x0000FFFFFFFFF000ULL

static void flush_line(void *p)
{
#ifdef __x86_64__
	__asm__ volatile("clflush (%0)" :: "r"(p) : "memory");
#else
	(void)p;
#endif
}

/* scratch[0]: the scratch page; scratch[1..3]: a PT, PD and PDP whose every entry
 * points one level down the scratch chain. Shared by all address spaces, never freed. */
static uint64_t scratch[4];

static uint64_t entry_to(uint64_t pa, int pte)
{
	return pa | PTE_PRESENT | PTE_RW | (pte ? PTE_PAT : 0);
}

/* a table (for level: 1 PT, 2 PD, 3 PDP, 4 PML4) with every entry on scratch */
static uint64_t new_table(int level)
{
	void *pg = buddy_alloc(0);
	if (!pg) return 0;
	uint64_t *t = (uint64_t *)PHYS_TO_VIRT((uint64_t)pg);
	uint64_t e = entry_to(scratch[level - 1], level == 1);
	for (int i = 0; i < 512; i++) t[i] = e;
	for (int i = 0; i < PAGE_SIZE; i += 64) flush_line((uint8_t *)t + i);
	return (uint64_t)pg;
}

static int scratch_init(void)
{
	if (scratch[3]) return 0;
	if (!scratch[0] && !(scratch[0] = intel_scratch_page())) return -ENOMEM_;
	for (int l = 1; l <= 3; l++)
		if (!scratch[l] && !(scratch[l] = new_table(l))) return -ENOMEM_;
	return 0;
}

static int is_scratch(uint64_t e) { uint64_t a = e & ADDR_MASK; return a == scratch[0] || a == scratch[1] || a == scratch[2] || a == scratch[3]; }

int ppgtt_init(ppgtt_t *vm)
{
	if (scratch_init()) return -ENOMEM_;
	vm->pml4 = new_table(4);
	return vm->pml4 ? 0 : -ENOMEM_;
}

/* the PTE for va, creating the tables on the way when alloc */
static uint64_t *walk(ppgtt_t *vm, uint64_t va, int alloc)
{
	uint64_t table = vm->pml4;
	for (int shift = 39, level = 3; shift >= 21; shift -= 9, level--) {
		uint64_t *t = (uint64_t *)PHYS_TO_VIRT(table);
		uint64_t *e = &t[(va >> shift) & 511];
		if (!(*e & PTE_PRESENT) || is_scratch(*e)) {
			if (!alloc) return NULL;
			uint64_t nt = new_table(level);
			if (!nt) return NULL;
			*e = nt | PTE_PRESENT | PTE_RW;
			flush_line(e);
		}
		table = *e & ADDR_MASK;
	}
	return &((uint64_t *)PHYS_TO_VIRT(table))[(va >> 12) & 511];
}

int ppgtt_map(ppgtt_t *vm, uint64_t va, drm_gem_t *o)
{
	for (uint32_t i = 0; i < o->npages; i++)
		if (!intel_phys_ok(o->pages[i])) return -EFAULT_;      /* never point the GPU outside RAM */
	for (uint32_t i = 0; i < o->npages; i++) {
		uint64_t *pte = walk(vm, va + (uint64_t)i * PAGE_SIZE, 1);
		if (!pte) { ppgtt_unmap(vm, va, (uint64_t)i * PAGE_SIZE); return -ENOMEM_; }
		*pte = o->pages[i] | PTE_PRESENT | PTE_RW | PTE_PAT;
		flush_line(pte);
	}
	return 0;
}

void ppgtt_unmap(ppgtt_t *vm, uint64_t va, uint64_t size)
{
	for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
		uint64_t *pte = walk(vm, va + off, 0);
		if (pte && !is_scratch(*pte)) { *pte = entry_to(scratch[0], 1); flush_line(pte); }
	}
}

static void free_level(uint64_t table, int level)
{
	uint64_t *t = (uint64_t *)PHYS_TO_VIRT(table);
	if (level > 1)
		for (int i = 0; i < 512; i++)
			if ((t[i] & PTE_PRESENT) && !is_scratch(t[i])) free_level(t[i] & ADDR_MASK, level - 1);
	pmm_free((void *)table);
}

void ppgtt_fini(ppgtt_t *vm)
{
	if (vm->pml4) free_level(vm->pml4, 4);    /* PML4 -> PDP -> PD -> PT (PTEs point at objects) */
	vm->pml4 = 0;
}
