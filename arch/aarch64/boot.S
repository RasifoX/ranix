.section ".text.boot"
.global _start

/*
 * ARM64 Linux kernel header — required so QEMU passes DTB in x0.
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

	mrs  x1, mpidr_el1
	and  x1, x1, #3
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
	ldr  x3, =0xFFFFFF8000000000
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
	ldr  x0, =0xFFFFFF8000000000
	add  sp, sp, x0

	/* jump to high-half kernel_main */
	ldr  x0, =kernel_main
	br   x0

.section ".data.boot", "aw"
.balign 8
.global _dtb_ptr
_dtb_ptr:
	.quad 0
