.global enable_mmu

enable_mmu:
    ldr x1, =0x000000000044FF00
    msr mair_el1, x1

    ldr x1, =0x5B510019
    msr tcr_el1, x1
    isb

    msr ttbr0_el1, x0
    isb

    ic  iallu
    tlbi vmalle1is
    dsb sy
    isb

    mrs x1, sctlr_el1
    orr x1, x1, #(1 << 0)
    orr x1, x1, #(1 << 2)
    orr x1, x1, #(1 << 12)
    msr sctlr_el1, x1
    isb

    ret