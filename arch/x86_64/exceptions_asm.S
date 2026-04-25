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
	sub  $512, %rsp
	fxsave (%rsp)
.endm

.macro restore_context
	fxrstor (%rsp)
	add  $512, %rsp
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

/* common helper for hardware IRQs (PIC 0x20-0x2F) */
common_irq:
	save_context
	mov  %rsp, %r15
	call task_current
	mov  %r15, 16(%rax)    /* task->ctx = rsp */
	
	/* get vector number from the stack (arch_context_t.vector) */
	/* offset = 512 (fxsave) + 120 (GPRs) = 632 */
	movq 632(%rsp), %rdi
	call irq_dispatch
	
	/* Check for reschedule */
	movl need_resched(%rip), %eax
	test %eax, %eax
	jz .irq_ret
	movl $0, need_resched(%rip)

	call schedule
	call vm_activate

.irq_ret:
	call sched_get_current_ctx
	mov  %rax, %rsp
	restore_context
	add  $16, %rsp
	iretq

.macro irq_stub vec
.global irq_stub_\vec
irq_stub_\vec:
	push $0
	push $0x\vec
	jmp  common_irq
.endm

irq_stub 20; irq_stub 21; irq_stub 22; irq_stub 23
irq_stub 24; irq_stub 25; irq_stub 26; irq_stub 27
irq_stub 28; irq_stub 29; irq_stub 2a; irq_stub 2b
irq_stub 2c; irq_stub 2d; irq_stub 2e; irq_stub 2f

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

/* syscall instruction entry: RAX=nr, RCX=user_rip, R11=user_rflags, RSP=user_rsp */
.section .data
.align 8
_syscall_usp: .quad 0

.section .text
.global syscall_entry
syscall_entry:
	movq %rsp, _syscall_usp(%rip)
	movq current_kstack_top(%rip), %rsp
	/* build iret-compatible frame */
	pushq $0x23                    /* ss  = GDT_USER_DATA */
	pushq _syscall_usp(%rip)       /* rsp = user RSP */
	pushq %r11                     /* rflags */
	pushq $0x1B                    /* cs  = GDT_USER_CODE */
	pushq %rcx                     /* rip = return address */
	pushq $0                       /* error_code */
	pushq $0x80                    /* vector */
	save_context
	mov  %rsp, %r15
	call task_current
	mov  %r15, 16(%rax)
	mov  %rsp, %rdi
	call syscall_handle
	/* reschedule if need_resched set (e.g. after vfork) */
	movl need_resched(%rip), %eax
	testl %eax, %eax
	jz   .syscall_ret
	movl $0, need_resched(%rip)
	call schedule
	call vm_activate
	call sched_get_current_ctx
	mov  %rax, %rsp
.syscall_ret:
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
