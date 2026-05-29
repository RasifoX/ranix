.arch armv8-a+fp+simd

/*
 * arch_context_t layout (800 bytes for 16-byte alignment):
 *   0   x0-x30  (248 bytes)
 *   248 sp
 *   256 elr_el1
 *   264 spsr_el1
 *   272 q0-q31  (512 bytes)
 *   784 fpsr
 *   788 fpcr
 *   792 padding (8 bytes)
 */

#define TF_SIZE 800

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
	mrs x0, sp_el0
	str x0, [sp, #248]
	mrs x1, elr_el1
	mrs x2, spsr_el1
	str x1, [sp, #256]
	str x2, [sp, #264]
	/* q0-q31 */
	stp q0,  q1,  [sp, #272]
	stp q2,  q3,  [sp, #304]
	stp q4,  q5,  [sp, #336]
	stp q6,  q7,  [sp, #368]
	stp q8,  q9,  [sp, #400]
	stp q10, q11, [sp, #432]
	stp q12, q13, [sp, #464]
	stp q14, q15, [sp, #496]
	stp q16, q17, [sp, #528]
	stp q18, q19, [sp, #560]
	stp q20, q21, [sp, #592]
	stp q22, q23, [sp, #624]
	stp q24, q25, [sp, #656]
	stp q26, q27, [sp, #688]
	stp q28, q29, [sp, #720]
	stp q30, q31, [sp, #752]
	mrs x0, fpsr
	mrs x1, fpcr
	str w0, [sp, #784]
	str w1, [sp, #788]
.endm

.macro restore_trapframe
	mov sp, x0
	/* q0-q31 */
	ldp q0,  q1,  [sp, #272]
	ldp q2,  q3,  [sp, #304]
	ldp q4,  q5,  [sp, #336]
	ldp q6,  q7,  [sp, #368]
	ldp q8,  q9,  [sp, #400]
	ldp q10, q11, [sp, #432]
	ldp q12, q13, [sp, #464]
	ldp q14, q15, [sp, #496]
	ldp q16, q17, [sp, #528]
	ldp q18, q19, [sp, #560]
	ldp q20, q21, [sp, #592]
	ldp q22, q23, [sp, #624]
	ldp q24, q25, [sp, #656]
	ldp q26, q27, [sp, #688]
	ldp q28, q29, [sp, #720]
	ldp q30, q31, [sp, #752]
	ldr w1, [sp, #784]
	ldr w2, [sp, #788]
	msr fpsr, x1
	msr fpcr, x2
	/* control regs */
	ldr x1, [sp, #256]
	ldr x2, [sp, #264]
	msr elr_el1, x1
	msr spsr_el1, x2

	tst  x2, #0xF
	b.ne .Lres_el1_\@

	/* returning to EL0 */
	ldr  x1, [sp, #248]
	msr  sp_el0, x1
	ldr  x1,      [sp, #8]
	ldp  x2,  x3, [sp, #16]
	ldp  x4,  x5, [sp, #32]
	ldp  x6,  x7, [sp, #48]
	ldp  x8,  x9, [sp, #64]
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
	ldr  x0, [sp, #0]
	add  sp, sp, #TF_SIZE
	eret

.Lres_el1_\@:
	/* returning to EL1 */
	ldr  x1,      [sp, #8]
	ldp  x2,  x3, [sp, #16]
	ldp  x4,  x5, [sp, #32]
	ldp  x6,  x7, [sp, #48]
	ldp  x8,  x9, [sp, #64]
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
	mov  x0, sp
	add  sp, x0, #TF_SIZE
	ldr  x0, [x0, #0]
	eret
.endm

.section ".vectors", "ax"
.balign 2048
.global exception_vectors
exception_vectors:
	/* EL1t — unused */
	b sync_handler
	.balign 128
	b irq_handler
	.balign 128
	b default_handler
	.balign 128
	b default_handler

	/* EL1h */
	.balign 128
	b sync_handler
	.balign 128
	b irq_handler
	.balign 128
	b irq_handler
	.balign 128
	b default_handler

	/* EL0 64-bit */
	.balign 128
	b sync_handler
	.balign 128
	b irq_handler
	.balign 128
	b irq_handler
	.balign 128
	b default_handler

	/* EL0 32-bit */
	.balign 128
	b default_handler
	.balign 128
	b default_handler
	.balign 128
	b default_handler
	.balign 128
	b default_handler

.global irq_handler
irq_handler:
	save_trapframe
	bl   task_current
	mov  x1, sp
	str  x1, [x0, #16]    /* task->ctx = sp */
	mov  w0, #0
	bl   irq_dispatch

	/* Check per-CPU need_resched (cpu_context_t offset 80) */
	mrs  x19, tpidr_el1
	ldr  w20, [x19, #80]
	cbz  w20, .irq_ret
	str  wzr, [x19, #80]

	bl   task_current
	mov  x1, sp
	str  x1, [x0, #16]

	msr  daifset, #2
	bl   schedule
	bl   vm_activate
	bl   sched_get_current_ctx
	mov  sp, x0
	bl   sched_requeue_prev
	bl   sched_free_dying_kstack

.irq_ret:
	mov  x0, sp
	bl   signal_dispatch
	bl   sched_get_current_ctx
	mov  sp, x0
	restore_trapframe
	eret

sync_handler:
	save_trapframe
	mrs  x0, esr_el1
	ubfx x1, x0, #26, #6
	cmp  x1, #0x15
	b.ne .sync_fault

	mrs  x2, spsr_el1
	tst  x2, #0xF
	b.ne .yield

	/* EL0 syscall */
	bl   task_current
	mov  x1, sp
	str  x1, [x0, #16]
	mov  x0, sp
	bl   syscall_handle

	/* Check per-CPU need_resched */
	mrs  x19, tpidr_el1
	ldr  w20, [x19, #80]
	cbz  w20, .syscall_ret
	str  wzr, [x19, #80]

	bl   task_current
	mov  x1, sp
	str  x1, [x0, #16]

	msr  daifset, #2
	bl   schedule
	bl   vm_activate
	bl   sched_get_current_ctx
	mov  sp, x0
	bl   sched_requeue_prev
	bl   sched_free_dying_kstack

.syscall_ret:
	mov  x0, sp
	bl   signal_dispatch
	bl   sched_get_current_ctx
	mov  sp, x0
	restore_trapframe
	eret

.yield:
	bl   task_current
	mov  x1, sp
	str  x1, [x0, #16]
	msr  daifset, #2
	bl   schedule
	bl   vm_activate
	bl   sched_get_current_ctx
	mov  sp, x0
	bl   sched_requeue_prev
	bl   sched_free_dying_kstack
	mov  x0, sp
	bl   signal_dispatch
	bl   sched_get_current_ctx
	mov  sp, x0
	restore_trapframe
	eret

.sync_fault:
	/* A fault can happen while the syscall/IRQ return path is mid-flight
	 * (e.g. signal_dispatch writing the signal frame triggers a user-stack
	 * grow). That outer path relies on task->ctx pointing at ITS trapframe.
	 * Save the current task->ctx, publish the fault frame only for the
	 * duration of sync_dispatch (so a fatal fault's reschedule sees it),
	 * then restore it and eret back to our own fault frame. Clobbering
	 * task->ctx here would make the outer path restore a stale frame and
	 * eret into a corrupted state — wedging the CPU. */
	bl   task_current
	ldr  x19, [x0, #16]      /* x19 = caller's saved ctx (callee-saved) */
	mov  x1, sp
	str  x1, [x0, #16]       /* task->ctx = fault frame */
	mov  x0, sp
	bl   sync_dispatch
	bl   task_current
	str  x19, [x0, #16]      /* restore caller's ctx */
	mov  x0, sp
	bl   signal_dispatch
	mov  x0, sp              /* return to our own fault frame */
	restore_trapframe
	eret

default_handler:
	b default_handler
