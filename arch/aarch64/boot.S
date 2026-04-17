.section ".text.boot"
.global _start
.global _dtb_ptr

_start:
    /* x0 = DTB pointer from firmware — save it */
    ldr x1, =_dtb_ptr
    str x0, [x1]

    mrs     x1, mpidr_el1
    and     x1, x1, #3
    cbz     x1, 2f
1:  wfe
    b       1b

2:
    /* ── drop from EL2 → EL1 if needed ─────────────────── */
    mrs x0, CurrentEL
    lsr x0, x0, #2
    cmp x0, #2
    b.ne .Lel1_entry

    /* EL2: configure EL1 environment before dropping */

    /* HCR_EL2: RW=1 (AArch64 at EL1), no HVC trapping */
    mov x0, #(1 << 31)       /* RW bit */
    msr hcr_el2, x0

    /* SCTLR_EL1: reset to safe defaults (M=0, C=0, I=0) */
    mov x0, #0
    msr sctlr_el1, x0

    /* SPSR_EL2: return to EL1h (0x5), all exceptions masked */
    mov x0, #0x3c5            /* D=1 A=1 I=1 F=1, M=EL1h */
    msr spsr_el2, x0

    /* ELR_EL2: return address = .Lel1_entry */
    adr x0, .Lel1_entry
    msr elr_el2, x0

    /* CNTHCTL_EL2: allow EL1 access to physical timer */
    mov x0, #3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr

    eret

.Lel1_entry:
    /* ── now at EL1 ────────────────────────────────────── */
    mov x0, #1
    msr spsel, x0
    isb

    ldr x1, =__stack_top
    mov sp, x1

    mrs x0, sctlr_el1
    bic x0, x0, #(1 << 0)
    bic x0, x0, #(1 << 2)
    bic x0, x0, #(1 << 12)
    msr sctlr_el1, x0
    isb

    ldr x1, =__bss_start
    ldr x2, =__bss_end
3:  cmp x1, x2
    b.ge 4f
    str xzr, [x1], #8
    b 3b

4:  bl kernel_main
    b 1b

.section ".data"
.balign 8
_dtb_ptr:
    .quad 0