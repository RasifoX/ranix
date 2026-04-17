/*
 * Exception vectors and trap frame save/restore for AArch64.
 *
 * Trapframe layout (272 bytes, matches trapframe_t in sched.h):
 *   [sp + 0]    x0 .. x30   (31 * 8 = 248 bytes)
 *   [sp + 248]  saved SP
 *   [sp + 256]  ELR_EL1
 *   [sp + 264]  SPSR_EL1
 *
 * On IRQ/SVC entry we save a full trapframe on the current kernel stack,
 * store its address in the current task's tf pointer, call C code
 * (which may call schedule()), then restore from current_task->tf.
 */

#define TF_SIZE 272   /* 34 * 8 */

/* ─── save trapframe on stack ─── */
.macro save_trapframe
    sub sp, sp, #TF_SIZE

    stp x0,  x1,  [sp, #0]
    stp x2,  x3,  [sp, #16]
    stp x4,  x5,  [sp, #32]
    stp x6,  x7,  [sp, #48]
    stp x8,  x9,  [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30,      [sp, #240]

    /* save SP (pre-exception), ELR, SPSR */
    add x0, sp, #TF_SIZE
    mrs x1, elr_el1
    mrs x2, spsr_el1
    str x0, [sp, #248]
    str x1, [sp, #256]
    str x2, [sp, #264]
.endm

/* ─── restore trapframe from address in x0 ─── */
.macro restore_trapframe
    /* x0 = pointer to trapframe to restore */
    mov sp, x0

    /* restore ELR, SPSR first */
    ldr x1, [sp, #256]
    ldr x2, [sp, #264]
    msr elr_el1, x1
    msr spsr_el1, x2

    ldp x0,  x1,  [sp, #0]
    ldp x2,  x3,  [sp, #16]
    ldp x4,  x5,  [sp, #32]
    ldp x6,  x7,  [sp, #48]
    ldp x8,  x9,  [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30,      [sp, #240]

    /* restore SP to post-trapframe position */
    add sp, sp, #TF_SIZE
.endm

/* ═══════════════════════════════════════════════════════════ */
/*  Vector table — must be 2048-byte aligned                  */
/* ═══════════════════════════════════════════════════════════ */
.section ".vectors", "ax"
.balign 2048
.global exception_vectors
exception_vectors:
    /* EL1t (SP_EL0) — shouldn't happen, we use SP_EL1 */
    b sync_handler
    .balign 128
    b irq_handler
    .balign 128
    b default_handler
    .balign 128
    b default_handler

    /* EL1h (SP_EL1) — this is what we use */
    .balign 128
    b sync_handler
    .balign 128
    b irq_handler
    .balign 128
    b default_handler
    .balign 128
    b default_handler

    /* EL0 64-bit — for future userspace */
    .balign 128
    b sync_handler
    .balign 128
    b irq_handler
    .balign 128
    b default_handler
    .balign 128
    b default_handler

    /* EL0 32-bit — not used */
    .balign 128
    b default_handler
    .balign 128
    b default_handler
    .balign 128
    b default_handler
    .balign 128
    b default_handler

/* raw UART debug: print char without touching stack */
.macro dbg_char ch
    movz x15, #0x1000
    movk x15, #0xFE20, lsl #16
    mov x16, #\ch
.Lwait_\@:
    ldr x17, [x15, #0x18]
    tbnz x17, #5, .Lwait_\@
    str x16, [x15]
.endm

/* ═══════════════════════════════════════════════════════════ */
/*  IRQ handler — preemptive scheduling happens here          */
/* ═══════════════════════════════════════════════════════════ */
.global irq_handler
irq_handler:
    save_trapframe

    /* store trapframe pointer in current task: task->tf = sp */
    bl  task_current         /* returns task_t* in x0 */
    mov x19, x0              /* save task_t* */
    mov x20, sp
    str x20, [x19, #16]       /* task->tf (offset 16) = sp */

    mov w0, #0
    bl  irq_dispatch

    /* schedule — may change current_task */
    bl  schedule

    bl  sched_get_current_ctx
    restore_trapframe

    eret

/* ═══════════════════════════════════════════════════════════ */
/*  SVC handler — voluntary yield / syscall                   */
/* ═══════════════════════════════════════════════════════════ */
sync_handler:
    save_trapframe

    /* check if this is an SVC (ESR_EL1.EC == 0x15) */
    mrs x0, esr_el1
    lsr x1, x0, #26
    cmp x1, #0x15
    b.ne .sync_not_svc

    /* SVC: store trapframe, schedule, restore */
    bl  task_current
    mov x1, sp
    str x1, [x0, #16]

    bl  schedule

    bl  sched_get_current_ctx
    restore_trapframe
    eret

.sync_not_svc:
    bl  sync_dispatch
    b   .

default_handler:
    b default_handler
