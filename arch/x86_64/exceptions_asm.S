.macro save_context
	pushq %rax
	pushq %rbx
	pushq %rcx
	pushq %rdx
	pushq %rsi
	pushq %rdi
	pushq %rbp
	pushq %r8
	pushq %r9
	pushq %r10
	pushq %r11
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	subq  $512, %rsp
	fxsave (%rsp)
.endm

.macro restore_context
	fxrstor (%rsp)
	addq  $512, %rsp
	popq  %r15
	popq  %r14
	popq  %r13
	popq  %r12
	popq  %r11
	popq  %r10
	popq  %r9
	popq  %r8
	popq  %rbp
	popq  %rdi
	popq  %rsi
	popq  %rdx
	popq  %rcx
	popq  %rbx
	popq  %rax
.endm

.macro exc_no_err vec
.global exc_stub_\vec
exc_stub_\vec:
	pushq $0
	pushq $\vec
	jmp  common_exc
.endm

.macro exc_has_err vec
.global exc_stub_\vec
exc_stub_\vec:
	pushq $\vec
	jmp  common_exc
.endm

exc_no_err  0;  exc_no_err  1;  exc_no_err  2;  exc_no_err  3
exc_no_err  4;  exc_no_err  5;  exc_no_err  6;  exc_no_err  7
exc_has_err 8;  exc_no_err  9;  exc_has_err 10; exc_has_err 11
exc_has_err 12; exc_has_err 13; exc_has_err 14; exc_no_err  15
exc_no_err  16; exc_has_err 17; exc_no_err  18; exc_no_err  19

common_exc:
	/* Before save_context: [RSP+0]=vec [RSP+8]=err [RSP+16]=RIP [RSP+24]=CS */
	testq $3, 24(%rsp)
	jz 1f
	swapgs
1:	save_context
	movq %rsp, %rdi
	call exc_dispatch
	restore_context
	addq $16, %rsp
	testq $3, 8(%rsp)           /* returning to user? */
	jz 2f
	swapgs
2:	iretq

/* common helper for hardware IRQs (PIC 0x20-0x2F) */
common_irq:
	/* Before save_context: [RSP+0]=vec [RSP+8]=err [RSP+16]=RIP [RSP+24]=CS */
	testq $3, 24(%rsp)
	jz 1f
	swapgs
1:	save_context
	movq %rsp, %r15
	call task_current
	movq %r15, 16(%rax)    /* task->ctx = rsp */

	/* get vector number from the stack (arch_context_t.vector) */
	movq 632(%rsp), %rdi
	call irq_dispatch

	/* Check for reschedule */
	movl need_resched(%rip), %eax
	testl %eax, %eax
	jz .irq_ret
	movl $0, need_resched(%rip)

	movq %rsp, %r15
	call task_current
	movq %r15, 16(%rax)

	call schedule
	call vm_activate

.irq_ret:
	/* Switch to current/new task's saved context before signal delivery */
	call sched_get_current_ctx
	movq %rax, %rsp
	movq %rsp, %rdi
	call signal_dispatch
	call sched_get_current_ctx
	movq %rax, %rsp
	restore_context
	addq $16, %rsp
	testq $3, 8(%rsp)
	jz 2f
	swapgs
2:	iretq

.macro irq_stub vec
.global irq_stub_\vec
irq_stub_\vec:
	pushq $0
	pushq $0x\vec
	jmp  common_irq
.endm

irq_stub 20; irq_stub 21; irq_stub 22; irq_stub 23
irq_stub 24; irq_stub 25; irq_stub 26; irq_stub 27
irq_stub 28; irq_stub 29; irq_stub 2a; irq_stub 2b
irq_stub 2c; irq_stub 2d; irq_stub 2e; irq_stub 2f

.global irq_stub_syscall
irq_stub_syscall:
	testq $3, 8(%rsp)
	jz 1f
	swapgs
1:	pushq $0
	pushq $0x80
	save_context
	movq %rsp, %r15
	call task_current
	movq %r15, 16(%rax)
	movq %rsp, %rdi
	call syscall_handle
	
	movq %rsp, %rdi
	call signal_dispatch

	call sched_get_current_ctx
	movq %rax, %rsp
	restore_context
	addq $16, %rsp
	testq $3, 8(%rsp)
	jz 2f
	swapgs
2:	iretq

/* syscall instruction entry: RAX=nr, RCX=user_rip, R11=user_rflags, RSP=user_rsp */
.section .text
.global syscall_entry
syscall_entry:
	swapgs                      /* get kernel GS */
	/* User RSP is in RSP register. Store it in per-CPU area safely. */
	movq %rsp, %gs:48           /* offset 48 is cpu_context.scratch */
	/* switch to kernel stack from per-CPU context offset 16 (kstack_top) */
	movq %gs:16, %rsp

	/* build trapframe */
	pushq $0x23                    /* ss */
	pushq %gs:48                   /* rsp (the saved user rsp) */
	pushq %r11                     /* rflags */
	pushq $0x1B                    /* cs */
	pushq %rcx                     /* rip */
	pushq $0                       /* error */
	pushq $0x80                    /* vector */
	save_context

	movq %rsp, %r15
	call task_current
	movq %r15, 16(%rax)            /* task->ctx = rsp */

	movq %rsp, %rdi
	call syscall_handle

	movq %rsp, %r15
	call task_current
	movq %r15, 16(%rax)

	/* signal dispatch */
	movq %rsp, %rdi
	call signal_dispatch

	/* reschedule check */
	movl need_resched(%rip), %eax
	testl %eax, %eax
	jz .syscall_ret
	movl $0, need_resched(%rip)

	movq %rsp, %r15
	call task_current
	movq %r15, 16(%rax)

	call schedule
	call vm_activate
	call sched_get_current_ctx
	movq %rax, %rsp

.syscall_ret:
	restore_context
	addq $16, %rsp
	swapgs
	iretq

.global irq_stub_yield
irq_stub_yield:
	testq $3, 8(%rsp)
	jz 1f
	swapgs
1:	pushq $0
	pushq $0x40
	save_context
	movq %rsp, %r15
	call task_current
	movq %r15, 16(%rax)
	call schedule
	call vm_activate

	/* Switch to new task's saved context before signal delivery */
	call sched_get_current_ctx
	movq %rax, %rsp
	movq %rsp, %rdi
	call signal_dispatch

	call sched_get_current_ctx
	movq %rax, %rsp
	restore_context
	addq $16, %rsp
	testq $3, 8(%rsp)
	jz 2f
	swapgs
2:	iretq
