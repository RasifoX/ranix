/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * S3: saving a CPU and bringing it back (arch/x86_64/sleep.c, asm/sleep.h).
 *
 * sleep_save_cpu(ctx) is setjmp with the system registers: it returns 0, and
 * returns 1 a second time when the CPU comes back through the wake page.
 *
 * The wake page (copied to WAKE_PHYS = 0x8000) is where the firmware jumps from
 * the FACS waking vector after S3, in real mode, and where an AP starts after
 * its SIPI. It goes to long mode exactly as trampoline.S does for an AP at
 * boot (A20, the same minimal GDT, PAE, LA57 when the CPU has it, NX, LME,
 * the kernel's top table) and jumps to sleep_resume_entry(ctx) in the high half.
 * The firmware sets CS:IP = 0800:0000 for a vector of 0x8000, an AP's SIPI
 * 0800:0000 too: the code uses only absolute addresses with DS = 0.
 */
#include <asm/sleep.h>

#define WD(off) (WAKE_PHYS + WAKE_DATA + (off))

.section ".rodata.sleepwake", "a"
.code16
.global sleep_wake_start
sleep_wake_start:
	cli
	cld
	xor  %ax, %ax
	mov  %ax, %ds
	mov  %ax, %es
	mov  %ax, %ss

	inb  $0x92, %al            /* A20 */
	or   $0x02, %al
	outb %al, $0x92

	lgdtl (WD(0x50))
	mov  %cr0, %eax
	or   $0x01, %eax
	mov  %eax, %cr0
	ljmp $0x08, $(WAKE_PHYS + .Lw32 - sleep_wake_start)

.code32
.Lw32:
	mov  $0x10, %ax
	mov  %ax, %ds
	mov  %ax, %es
	mov  %ax, %ss
	xor  %ax, %ax
	mov  %ax, %fs
	mov  %ax, %gs

	mov  %cr4, %eax
	or   $0x20, %eax           /* PAE */
	mov  %eax, %cr4

	mov  $7, %eax
	xor  %ecx, %ecx
	cpuid
	test $(1 << 16), %ecx
	jz   .Lw_no_la57
	mov  %cr4, %eax
	or   $(1 << 12), %eax      /* LA57: the kernel's table is a PML5 then */
	mov  %eax, %cr4
.Lw_no_la57:

	mov  $0x80000001, %eax
	cpuid
	and  $(1 << 20), %edx      /* NX */
	mov  %edx, %edi

	mov  $(WD(WD_TOP)), %ebx
	mov  (%ebx), %eax
	mov  %eax, %cr3

	mov  $0xC0000080, %ecx
	rdmsr
	or   $0x100, %eax          /* LME */
	test %edi, %edi
	jz   1f
	or   $0x800, %eax          /* NXE */
1:	wrmsr

	mov  %cr0, %eax
	or   $0x80000001, %eax     /* PG | PE */
	mov  %eax, %cr0
	ljmp $0x18, $(WAKE_PHYS + .Lw64 - sleep_wake_start)

.code64
.Lw64:
	mov  $0x20, %ax
	mov  %ax, %ds
	mov  %ax, %es
	mov  %ax, %ss
	movabsq $(WD(WD_CTX)), %rax
	movq (%rax), %rdi
	movabsq $(WD(WD_ENTRY)), %rax
	movq (%rax), %rax
	jmp  *%rax

.org WAKE_DATA
	.quad 0                    /* WD_TOP */
	.quad 0                    /* WD_CTX */
	.quad 0                    /* WD_ENTRY */
	.quad 0
.org (WAKE_DATA + 0x20)
	.quad 0
	.quad 0x00CF9A000000FFFF   /* 0x08 32-bit code */
	.quad 0x00CF92000000FFFF   /* 0x10 32-bit data */
	.quad 0x00AF9A000000FFFF   /* 0x18 64-bit code */
	.quad 0x00AF92000000FFFF   /* 0x20 64-bit data */
.org (WAKE_DATA + 0x50)
	.word (5 * 8 - 1)
	.long (WAKE_PHYS + WAKE_DATA + 0x20)
.global sleep_wake_end
sleep_wake_end:

.text
.code64

/* int sleep_save_cpu(sleep_cpu_t *ctx) */
.global sleep_save_cpu
sleep_save_cpu:
	movq %rbx, SC_RBX(%rdi)
	movq %rbp, SC_RBP(%rdi)
	movq %r12, SC_R12(%rdi)
	movq %r13, SC_R13(%rdi)
	movq %r14, SC_R14(%rdi)
	movq %r15, SC_R15(%rdi)
	leaq 8(%rsp), %rax
	movq %rax, SC_RSP(%rdi)
	movq (%rsp), %rax
	movq %rax, SC_RIP(%rdi)
	pushfq
	popq %rax
	movq %rax, SC_RFLAGS(%rdi)
	movq %cr0, %rax
	movq %rax, SC_CR0(%rdi)
	movq %cr3, %rax
	movq %rax, SC_CR3(%rdi)
	movq %cr4, %rax
	movq %rax, SC_CR4(%rdi)

	.macro save_msr msr, off
	movl $\msr, %ecx
	rdmsr
	shlq $32, %rdx
	orq  %rdx, %rax
	movq %rax, \off(%rdi)
	.endm
	save_msr 0xC0000080, SC_EFER
	save_msr 0xC0000100, SC_FSBASE
	save_msr 0xC0000101, SC_GSBASE
	save_msr 0xC0000102, SC_KGSBASE
	save_msr 0x277,      SC_PAT

	sgdt SC_GDTR(%rdi)
	sidt SC_IDTR(%rdi)

	movq %cr4, %rax
	testq $(1 << 18), %rax     /* OSXSAVE */
	jz   .Lsave_fx
	xorl %ecx, %ecx
	xgetbv
	shlq $32, %rdx
	orq  %rdx, %rax
	movq %rax, SC_XCR0(%rdi)
	movl $-1, %eax
	movl $-1, %edx
	xsave64 SC_FPU(%rdi)
	jmp  .Lsave_done
.Lsave_fx:
	movq $0, SC_XCR0(%rdi)
	fxsave64 SC_FPU(%rdi)
.Lsave_done:
	xorl %eax, %eax
	ret

/* sleep_resume_entry: rdi = ctx, IRQs off, the kernel's top table loaded by the
 * wake page. Control registers, descriptor tables, the MSRs the kernel set, the
 * FPU, then the saved stack: sleep_save_cpu returns 1. The TSS is loaded again
 * by the caller (gdt_init clears its busy bit first). */
.global sleep_resume_entry
sleep_resume_entry:
	movq SC_CR4(%rdi), %rax
	movq %rax, %cr4
	movq SC_CR0(%rdi), %rax
	movq %rax, %cr0
	movq SC_CR3(%rdi), %rax
	movq %rax, %cr3
	lgdt SC_GDTR(%rdi)
	lidt SC_IDTR(%rdi)
	pushq $0x08
	leaq 1f(%rip), %rax
	pushq %rax
	lretq
1:	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %ss
	xorw %ax, %ax
	movw %ax, %fs
	movw %ax, %gs

	.macro load_msr msr, off
	movl $\msr, %ecx
	movq \off(%rdi), %rax
	movq %rax, %rdx
	shrq $32, %rdx
	wrmsr
	.endm
	load_msr 0xC0000080, SC_EFER
	load_msr 0xC0000100, SC_FSBASE
	load_msr 0xC0000101, SC_GSBASE
	load_msr 0xC0000102, SC_KGSBASE
	load_msr 0x277,      SC_PAT

	movq SC_XCR0(%rdi), %rax
	testq %rax, %rax
	jz   .Lres_fx
	movq %rax, %rdx
	shrq $32, %rdx
	xorl %ecx, %ecx
	xsetbv
	movl $-1, %eax
	movl $-1, %edx
	xrstor64 SC_FPU(%rdi)
	jmp  .Lres_regs
.Lres_fx:
	fxrstor64 SC_FPU(%rdi)
.Lres_regs:
	movq SC_RBX(%rdi), %rbx
	movq SC_RBP(%rdi), %rbp
	movq SC_R12(%rdi), %r12
	movq SC_R13(%rdi), %r13
	movq SC_R14(%rdi), %r14
	movq SC_R15(%rdi), %r15
	movq SC_RSP(%rdi), %rsp
	pushq SC_RFLAGS(%rdi)
	popfq
	movl $1, %eax
	jmpq *SC_RIP(%rdi)
