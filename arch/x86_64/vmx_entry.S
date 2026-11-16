/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Into a VT-x guest and back (arch/x86_64/vmx.c).
 *
 * int vmx_enter(vmx_gregs_t *g, int launched)
 *   Loads the guest's general registers and CR2 from *g and runs the guest
 *   (VMLAUNCH the first time after VMPTRLD, VMRESUME after). Interrupts are
 *   off. Returns 0 after a VM exit with the guest's registers stored back in
 *   *g, or 1 when the instruction itself failed (VMfail: the guest never ran).
 *
 * A VM exit lands on vmx_exit_entry (HOST_RIP) with the stack pointer that
 * vmx_enter wrote to HOST_RSP, where it left the address of *g. Everything
 * else the host needs back is in the VMCS host-state area or callee-saved on
 * this stack.
 *
 * Provenance: VMLAUNCH/VMRESUME/VM-exit behaviour from the Intel SDM Vol. 3,
 * chapters 26-27. Original code. */

#define HOST_RSP 0x6C14

/* vmx_gregs_t: r[16] by register number, then cr2 */
#define G_AX   0
#define G_CX   8
#define G_DX   16
#define G_BX   24
#define G_BP   40
#define G_SI   48
#define G_DI   56
#define G_R8   64
#define G_R9   72
#define G_R10  80
#define G_R11  88
#define G_R12  96
#define G_R13  104
#define G_R14  112
#define G_R15  120
#define G_CR2  128

	.text
	.globl vmx_enter
	.type  vmx_enter, @function
vmx_enter:
	pushq %rbp
	movq  %rsp, %rbp
	pushq %rbx
	pushq %r12
	pushq %r13
	pushq %r14
	pushq %r15
	pushq %rdi                  /* *g, found again at the exit */
	movq  $HOST_RSP, %rax
	vmwrite %rsp, %rax
	jbe   .Lfail

	movq  G_CR2(%rdi), %rax
	movq  %rax, %cr2
	/* The flags of this test choose the instruction below: the movs that load
	 * the guest's registers leave them alone. */
	testl %esi, %esi
	movq  G_AX(%rdi),  %rax
	movq  G_CX(%rdi),  %rcx
	movq  G_DX(%rdi),  %rdx
	movq  G_BX(%rdi),  %rbx
	movq  G_BP(%rdi),  %rbp
	movq  G_SI(%rdi),  %rsi
	movq  G_R8(%rdi),  %r8
	movq  G_R9(%rdi),  %r9
	movq  G_R10(%rdi), %r10
	movq  G_R11(%rdi), %r11
	movq  G_R12(%rdi), %r12
	movq  G_R13(%rdi), %r13
	movq  G_R14(%rdi), %r14
	movq  G_R15(%rdi), %r15
	movq  G_DI(%rdi),  %rdi
	jz    1f
	vmresume
	jmp   .Lfail
1:	vmlaunch

.Lfail:
	/* VMfail: still on the host stack, the guest values in the registers are
	 * dropped. */
	popq  %rdi
	popq  %r15
	popq  %r14
	popq  %r13
	popq  %r12
	popq  %rbx
	popq  %rbp
	movl  $1, %eax
	ret
	.size vmx_enter, . - vmx_enter

	.globl vmx_exit_entry
	.type  vmx_exit_entry, @function
vmx_exit_entry:
	pushq %rdi                  /* the guest's rdi */
	movq  8(%rsp), %rdi         /* *g */
	movq  %rax, G_AX(%rdi)
	movq  %rcx, G_CX(%rdi)
	movq  %rdx, G_DX(%rdi)
	movq  %rbx, G_BX(%rdi)
	movq  %rbp, G_BP(%rdi)
	movq  %rsi, G_SI(%rdi)
	movq  %r8,  G_R8(%rdi)
	movq  %r9,  G_R9(%rdi)
	movq  %r10, G_R10(%rdi)
	movq  %r11, G_R11(%rdi)
	movq  %r12, G_R12(%rdi)
	movq  %r13, G_R13(%rdi)
	movq  %r14, G_R14(%rdi)
	movq  %r15, G_R15(%rdi)
	popq  %rax
	movq  %rax, G_DI(%rdi)
	movq  %cr2, %rax
	movq  %rax, G_CR2(%rdi)
	popq  %rdi
	popq  %r15
	popq  %r14
	popq  %r13
	popq  %r12
	popq  %rbx
	popq  %rbp
	xorl  %eax, %eax
	ret
	.size vmx_exit_entry, . - vmx_exit_entry
