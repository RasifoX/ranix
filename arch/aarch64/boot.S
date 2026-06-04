.section ".text.boot"
.global _start

/*
 * ARM64 kernel image header — required so QEMU passes the DTB in x0.
 * code0 = branch over header; magic 0x644d5241 at offset 0x38.
 */
_start:
	b    _start_code
	.long 0
	.quad 0x80000
	.quad 0
	.quad 0
	.quad 0
	.quad 0
	.quad 0
	.long 0x644d5241
	.long 0

_start_code:
	/* x0 = DTB pointer from firmware */
	adrp x1, _dtb_ptr
	add  x1, x1, :lo12:_dtb_ptr
	str  x0, [x1]

	/* Park every CPU but the primary. The primary is the one whose full MPIDR
	 * affinity (Aff3.Aff2.Aff1.Aff0) is zero — masking only Aff0 (the old
	 * `& 3`) would also pass cluster1-core0 et al. on a multi-cluster part and
	 * boot two "primaries". MPIDR_HWID_BITMASK = 0xff00ffffff (Linux). */
	mrs  x1, mpidr_el1
	movz x2, #0xffff
	movk x2, #0x00ff, lsl #16
	movk x2, #0x00ff, lsl #32
	and  x1, x1, x2
	cbz  x1, 2f
1:	wfe
	b    1b

2:
	/* ── drop from EL2 → EL1 if needed ─────────────────── */
	mrs x0, CurrentEL
	lsr x0, x0, #2
	cmp x0, #2
	b.ne .Lel1_entry

	mov x0, #(1 << 31)
	msr hcr_el2, x0

	mov x0, #0
	msr sctlr_el1, x0

	mov x0, #0x3c5
	msr spsr_el2, x0

	adr x0, .Lel1_entry
	msr elr_el2, x0

	mov x0, #3
	msr cnthctl_el2, x0
	msr cntvoff_el2, xzr

	eret

.Lel1_entry:
	mov x0, #1
	msr spsel, x0
	isb

	/* stack is at physical address (linker puts __stack_top in boot section) */
	adrp x1, __stack_top
	add  x1, x1, :lo12:__stack_top
	mov  sp, x1

	mrs x0, sctlr_el1
	bic x0, x0, #(1 << 0)
	bic x0, x0, #(1 << 2)
	bic x0, x0, #(1 << 12)
	msr sctlr_el1, x0
	isb

	/* clear .bss (at physical addresses — subtract VIRT_OFFSET from VMA) */
	ldr  x3, =0xFFFF800000000000
	ldr  x1, =__bss_start
	ldr  x2, =__bss_end
	sub  x1, x1, x3
	sub  x2, x2, x3
3:	cmp  x1, x2
	b.ge 4f
	str  xzr, [x1], #8
	b    3b

4:
	/* set up page tables and enable MMU */
	bl   mmu_tables_init

	/* move SP from physical to TTBR1 virtual — kernel stack must be in high half */
	ldr  x0, =0xFFFF800000000000
	add  sp, sp, x0

	/* jump to high-half kernel_main */
	ldr  x0, =kernel_main
	br   x0

/*
 * Secondary CPU trampoline — runs at physical address with MMU off.
 * x0 = cpu_id (passed as PSCI context_id)
 */
.section ".text.boot", "ax"
.global secondary_trampoline
secondary_trampoline:
	/* Drop EL2 → EL1 if needed (same as primary) */
	mrs x1, CurrentEL
	lsr x1, x1, #2
	cmp x1, #2
	b.ne .Lsec_el1

	mov x1, #(1 << 31)
	msr hcr_el2, x1
	mov x1, #0
	msr sctlr_el1, x1       /* known-clean EL1 state (EE=0 etc.), like primary */
	mov x1, #0x3c5
	msr spsr_el2, x1
	adr x1, .Lsec_el1
	msr elr_el2, x1
	mov x1, #3
	msr cnthctl_el2, x1
	msr cntvoff_el2, xzr

#ifdef BOARD_RASPI4
	/* Cortex-A72: join the SMP coherency domain (CPUECTLR_EL1.SMPEN, bit 6)
	 * while still at EL2 — EL1 access to this impl-def register traps into
	 * our empty EL2 vector. Must run before enable_mmu so the cacheable
	 * page-table walk sees the primary's tables. Real-hardware only. */
	mrs x1, S3_1_C15_C2_1
	orr x1, x1, #(1 << 6)
	msr S3_1_C15_C2_1, x1
	isb
#endif

	eret

.Lsec_el1:
	mov x9, #1
	msr spsel, x9
	isb

	/* Derive cpu_id from MPIDR Aff0 (core number). Works for both PSCI
	 * (QEMU virt) and spin-table (real Pi4) entry — spin-table does not
	 * pass a context_id in x0, so MPIDR is the reliable source. Aff0 is
	 * 0..3 on both BCM2711 (single cluster) and QEMU virt. */
	mrs x20, mpidr_el1
	and x20, x20, #0xff

	/* Disable MMU + caches to a known state before (re)configuring them.
	 * The firmware armstub may hand the secondary over with the MMU and/or
	 * caches enabled; reprogramming TTBR/TCR with translation live faults.
	 * The primary does the same in .Lel1_entry. */
	mrs x3, sctlr_el1
	bic x3, x3, #(1 << 0)    /* M  — MMU */
	bic x3, x3, #(1 << 2)    /* C  — data cache */
	bic x3, x3, #(1 << 12)   /* I  — instruction cache */
	msr sctlr_el1, x3
	isb

	/* Read boot params from ap_boot_sync[cpu_id]:
	 * struct ap_boot_sync layout (40 bytes per entry):
	 *   offset 0:  ttbr0_phys (u64)
	 *   offset 8:  ttbr1_phys (u64)
	 *   offset 16: sp_phys    (u64)
	 *   offset 24: cpu_id     (i32)
	 *   offset 28: ready      (i32)
	 *   offset 32: go         (i32)
	 *   offset 36: _pad       (i32)
	 */
	adr  x10, ap_boot_sync
	mov  x11, #40
	mul  x11, x20, x11
	add  x10, x10, x11   /* x10 = &ap_boot_sync[cpu_id] */

	ldr  x0, [x10, #0]   /* ttbr0_phys */
	ldr  x1, [x10, #8]   /* ttbr1_phys */

	/* Set up physical boot stack before enabling MMU */
	ldr  x2, [x10, #16]  /* sp_phys */
	mov  sp, x2

	/* Drop stale clean cache lines for the page tables this core never
	 * wrote: kernel_l1 (x0), kernel_l1_high (x1), and the shared L2 it
	 * points at. Without this the cacheable table walk reads stale lines
	 * and faults the instant the MMU comes on. */
	mov  x9, x0
	mov  x10, #0x1000
	bl   inval_dcache_region
	mov  x9, x1
	mov  x10, #0x1000
	bl   inval_dcache_region
	ldr  x9, [x0]                       /* kernel_l1[0] → L2 descriptor */
	and  x9, x9, #0x0000fffffffff000    /* mask to L2 physical base */
	mov  x10, #0x1000
	bl   inval_dcache_region

	bl   enable_mmu

	/* MMU on — convert physical SP to virtual */
	ldr  x1, =0xFFFF800000000000
	add  sp, sp, x1

	mov  x0, x20         /* cpu_id */
	ldr  x9, =secondary_cpu_main
	br   x9

.section ".data.boot", "aw"
.balign 8
.global _dtb_ptr
_dtb_ptr:
	.quad 0
