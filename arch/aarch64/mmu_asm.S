.section ".text.boot", "ax"
.global enable_mmu

/* enable_mmu(uint64_t ttbr0_phys, uint64_t ttbr1_phys) */
enable_mmu:
	ldr x2, =0x000000000044FF00
	msr mair_el1, x2

	ldr x2, =0xB7103510		/* T0SZ=16 T1SZ=16 (48-bit VA, 4-level) TG0=4KB TG1=4KB */
	msr tcr_el1, x2
	isb

	msr ttbr0_el1, x0
	msr ttbr1_el1, x1
	isb

	ic  iallu
	tlbi vmalle1is
	dsb sy
	isb

	mrs x2, sctlr_el1
	orr x2, x2, #(1 << 0)
	orr x2, x2, #(1 << 2)
	orr x2, x2, #(1 << 12)
	msr sctlr_el1, x2
	isb

	ret

/*
 * inval_dcache_region(x9 = start, x10 = size) — invalidate by VA to PoC.
 * Clobbers x9-x13. Used by secondaries to drop stale clean lines for the
 * page tables (primary wrote them caches-off, so no coherency snoop reached
 * this core) before turning the MMU on with cacheable table walks.
 */
.global inval_dcache_region
inval_dcache_region:
	add  x10, x9, x10
	mrs  x11, ctr_el0
	ubfx x11, x11, #16, #4   /* DminLine: log2(words per line) */
	mov  x12, #4
	lsl  x12, x12, x11       /* line size in bytes */
	sub  x13, x12, #1
	bic  x9, x9, x13         /* align start down to a line */
1:	dc   ivac, x9
	add  x9, x9, x12
	cmp  x9, x10
	b.lo 1b
	dsb  sy
	isb
	ret
