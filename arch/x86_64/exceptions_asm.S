/*
 * arch_context_t layout on stack (matches asm/context.h):
 *   r15-r8, rbp, rdi, rsi, rdx, rcx, rbx, rax  (user-pushed, 120 bytes)
 *   vector, error_code                           (stub-pushed, 16 bytes)
 *   rip, cs, rflags, rsp, ss                     (cpu-pushed,  40 bytes)
 */

.macro save_context
	push %rax
	push %rbx
	push %rcx
	push %rdx
	push %rsi
	push %rdi
	push %rbp
	push %r8
	push %r9
	push %r10
	push %r11
	push %r12
	push %r13
	push %r14
	push %r15
.endm

.macro restore_context
	pop %r15
	pop %r14
	pop %r13
	pop %r12
	pop %r11
	pop %r10
	pop %r9
	pop %r8
	pop %rbp
	pop %rdi
	pop %rsi
	pop %rdx
	pop %rcx
	pop %rbx
	pop %rax
.endm

.macro exc_no_err vec
.global exc_stub_\vec
exc_stub_\vec:
	push $0
	push $\vec
	jmp  common_exc
.endm

.macro exc_has_err vec
.global exc_stub_\vec
exc_stub_\vec:
	push $\vec
	jmp  common_exc
.endm

exc_no_err  0;  exc_no_err  1;  exc_no_err  2;  exc_no_err  3
exc_no_err  4;  exc_no_err  5;  exc_no_err  6;  exc_no_err  7
exc_has_err 8;  exc_no_err  9;  exc_has_err 10; exc_has_err 11
exc_has_err 12; exc_has_err 13; exc_has_err 14; exc_no_err  15
exc_no_err  16; exc_has_err 17; exc_no_err  18; exc_no_err  19

common_exc:
	save_context
	mov %rsp, %rdi
	call exc_dispatch
	restore_context
	add $16, %rsp
	iretq

.global irq_stub_0
irq_stub_0:
	push $0
	push $0x20
	save_context
	mov  %rsp, %r15
	call task_current
	mov  %r15, 16(%rax)    /* task->ctx = rsp */
	mov  $0x20, %edi
	call irq_dispatch
	call schedule
	call vm_activate
	call sched_get_current_ctx
	mov  %rax, %rsp
	restore_context
	add  $16, %rsp
	iretq

.global irq_stub_syscall
irq_stub_syscall:
	push $0
	push $0x80
	save_context
	mov  %rsp, %r15
	call task_current
	mov  %r15, 16(%rax)
	mov  %rsp, %rdi         /* arg: arch_context_t* */
	call syscall_handle
	call sched_get_current_ctx
	mov  %rax, %rsp
	restore_context
	add  $16, %rsp
	iretq

.global irq_stub_yield
irq_stub_yield:
	push $0
	push $0x40
	save_context
	mov  %rsp, %r15
	call task_current
	mov  %r15, 16(%rax)
	call schedule
	call vm_activate
	call sched_get_current_ctx
	mov  %rax, %rsp
	restore_context
	add  $16, %rsp
	iretq
