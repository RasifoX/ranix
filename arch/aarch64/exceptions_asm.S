/*
 * arch_context_t layout (272 bytes, fixed — offsets hardcoded below):
 *   0   x0-x30  (248 bytes)
 *   248 sp
 *   256 elr_el1
 *   264 spsr_el1
 */

#define TF_SIZE 272

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
	add x0, sp, #TF_SIZE
	mrs x1, elr_el1
	mrs x2, spsr_el1
	str x0, [sp, #248]
	str x1, [sp, #256]
	str x2, [sp, #264]
.endm

.macro restore_trapframe
	mov sp, x0
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
	add sp, sp, #TF_SIZE
.endm

.section ".vectors", "ax"
.balign 2048
.global exception_vectors
exception_vectors:
	/* EL1t — unused, we always run on SP_EL1 */
	b sync_handler
	.balign 128
	b irq_handler
	.balign 128
	b default_handler
	.balign 128
	b default_handler

	/* EL1h — active exception level */
	.balign 128
	b sync_handler
	.balign 128
	b irq_handler
	.balign 128
	b default_handler
	.balign 128
	b default_handler

	/* EL0 64-bit */
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

/* emergency UART putc without touching the stack */
.macro dbg_char ch
	movz x15, #0x1000
	movk x15, #0xFE20, lsl #16
	mov  x16, #\ch
.Lwait_\@:
	ldr  x17, [x15, #0x18]
	tbnz x17, #5, .Lwait_\@
	str  x16, [x15]
.endm

.global irq_handler
irq_handler:
	save_trapframe
	bl   task_current
	mov  x19, x0
	mov  x20, sp
	str  x20, [x19, #16]    /* task->ctx = sp */
	mov  w0, #0
	bl   irq_dispatch
	bl   schedule
	bl   vm_activate
	bl   sched_get_current_ctx
	restore_trapframe
	eret

sync_handler:
	save_trapframe
	mrs  x0, esr_el1
	ubfx x1, x0, #26, #6   /* EC = esr[31:26] */
	cmp  x1, #0x15
	b.ne .sync_fault

	/* EC=0x15: SVC — check immediate (ISS = esr[15:0]) */
	and  x2, x0, #0xFFFF
	cbnz x2, .syscall

	/* SVC #0: yield */
	bl   task_current
	mov  x1, sp
	str  x1, [x0, #16]
	bl   schedule
	bl   vm_activate
	bl   sched_get_current_ctx
	restore_trapframe
	eret

.syscall:
	/* SVC #1: syscall from user */
	bl   task_current
	mov  x1, sp
	str  x1, [x0, #16]
	mov  x0, sp              /* arg: arch_context_t* */
	bl   syscall_handle
	bl   sched_get_current_ctx
	restore_trapframe
	eret

.sync_fault:
	bl   sync_dispatch
	b    .

default_handler:
	b default_handler
