/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/kprintf.h>
#include <kernel/mm/mmu.h>
#include <kernel/mm/vm.h>
#include <kernel/lib/string.h>
#include <asm/mmu.h>
#include <kernel/extable.h>

/* 1 if a kernel read of va faulted (recovered through the exception table) */
static int kernel_read_faults(uint64_t va)
{
	int ok = 0;
	uint64_t v = 0;
	__asm__ volatile("1: movq (%2), %1\n"
	                 "   movl $1, %0\n"
	                 "2:\n"
	                 _ASM_EXTABLE("1b", "2b")
	                 : "+r"(ok), "+r"(v) : "r"(va) : "memory");
	return !ok;
}

/* A NULL dereference in the kernel must fault, not reach physical page 0.
 * Run once the IDT and the per-CPU task are up: the fault path needs both. */
void mmu_null_guard_selftest(void)
{
	if (kernel_read_faults(0))
		kprintf(KERN_INFO "mm: null page guard ok\n");
	else
		kprintf(KERN_ERR "mm: null page guard MISSING: kernel VA 0 is mapped\n");
}

/* The boot identity map (pml4[0] -> pdpt[0] -> pd, 2 MB pages) also covers
 * VA 0, so a kernel NULL dereference read or wrote physical page 0 (the real-
 * mode IVT) instead of faulting. Its first 2 MB move to 4 KB pages with page 0
 * left out. pd is shared with the kernel-text mapping at -2 GB, whose first
 * page is as unused. Done on the BSP before the APs start: none of them has
 * the old entry cached, and the AP trampoline (0x7000) stays mapped. */
#define NG_PA_MASK    0x000FFFFFFFFFF000UL
#define NG_PA_2M_MASK 0x000FFFFFFFE00000UL
#define NG_PTE_PS     (1UL << 7)
static uint64_t null_guard_pt[512] __attribute__((aligned(4096)));

static void null_guard_install(void)
{
	uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[0] & NG_PA_MASK);
	if (!(pml4[0] & 1) || !(pdpt[0] & 1)) return;
	uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[0] & NG_PA_MASK);
	if (!(pd[0] & 1) || !(pd[0] & NG_PTE_PS)) return;   /* not the boot 2 MB page */
	uint64_t base = pd[0] & NG_PA_2M_MASK;
	null_guard_pt[0] = 0;
	for (int i = 1; i < 512; i++)
		null_guard_pt[i] = (base + (uint64_t)i * PAGE_SIZE) | 0x3UL;   /* P + RW, as the 2 MB page */
	pd[0] = VIRT_TO_PHYS((uint64_t)null_guard_pt) | 0x3UL;
	__asm__ volatile("invlpg (%0)" :: "r"(0UL) : "memory");
	__asm__ volatile("invlpg (%0)" :: "r"(0xFFFFFFFF80000000UL) : "memory");
}

void mmu_init(void)
{
	/* Read the boot LA57 flag while the identity map is still live, and cache
	 * it in the high half for the fault-path code (see arch_vm_set_la57). */
	int la57 = la57_enabled;
	arch_vm_set_la57(la57);

	/* Kernel address space root the CPU loads into CR3: PML5 under 5-level. */
	uint64_t root = la57 ? (uint64_t)pml5 : (uint64_t)pml4;
	vm_init_kernel(root);
	vm_set_kernel_pt(root);

	kprintf(KERN_INFO "mm: paging %s\n", la57 ? "5-level (LA57, 128 PB VA)" : "4-level (256 TB VA)");
	null_guard_install();
}

/* Direct map covers PML4[256] (boot pdpt_dm, first 512 GB). To map RAM past
 * 512 GB we hand out more PDPTs from this pool, one per extra 512 GB slot, up
 * to PML4[510] (leaving PML4[511] for kernel text / ioremap) — i.e. ~127 TB in
 * 4-level paging. Beyond that needs 5-level (LA57): a separate, deeper rework.
 * NOLOAD .bss, so the pool costs nothing in the binary. */
#define DM_EXTRA_PDPTS 254   /* PML4[257..510] */
static uint64_t dm_pool[DM_EXTRA_PDPTS][512] __attribute__((aligned(4096)));

void arch_map_high_memory(uint64_t max_phys)
{
	/* 1GB huge pages (PDPE1GB) — no PD allocation, no buddy dependency. */
	for (uint64_t phys = 4ULL << 30; phys < max_phys; phys += 1ULL << 30) {
		uint64_t pml4i = 256 + (phys >> 39);          /* 512 GB per PML4 slot */
		if (pml4i > 510) break;                       /* VA / kernel-region cap */
		uint64_t pdpti = (phys >> 30) & 0x1FF;

		uint64_t *pdpt;
		if (pml4i == 256) {
			pdpt = pdpt_dm;                           /* boot-provided PDPT */
		} else if (pml4[pml4i] & 1) {
			pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4i] & ~0xFFFUL);
		} else {
			pdpt = dm_pool[pml4i - 257];
			pml4[pml4i] = VIRT_TO_PHYS((uint64_t)pdpt) | 0x3UL;  /* P + RW */
		}
		if (pdpt[pdpti] & 1) continue;
		pdpt[pdpti] = phys | 0x83UL | (1UL << 63);    /* P + RW + PS(1GB) + NX (data only) */
	}
}
