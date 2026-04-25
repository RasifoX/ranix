.section ".text.boot", "ax"
.global enable_mmu

/* enable_mmu(uint64_t ttbr0_phys, uint64_t ttbr1_phys) */
enable_mmu:
	ldr x2, =0x000000000044FF00
	msr mair_el1, x2

	ldr x2, =0xB7193519		/* T0SZ=25 T1SZ=25 TG0=4KB TG1=4KB */
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
