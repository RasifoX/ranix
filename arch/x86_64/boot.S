/*
 * x86_64 boot stub. GRUB loads us in 32-bit protected mode (multiboot2).
 * Sets up identity-mapped page tables, switches to long mode, calls kernel_main.
 */

.set MB2_MAGIC,    0xe85250d6
.set MB2_ARCH,     0
.set MB2_LEN,      (mb2_end - mb2_start)
.set MB2_CHECKSUM, -(MB2_MAGIC + MB2_ARCH + MB2_LEN)

.section ".multiboot2", "a"
.balign 8
mb2_start:
	.long MB2_MAGIC
	.long MB2_ARCH
	.long MB2_LEN
	.long MB2_CHECKSUM
	.word 0; .word 0; .long 8
mb2_end:

.section ".text.boot"
.code32
.global _start
_start:
	cli
	mov  $boot_stack32_top, %esp
	mov  $__bss_start, %edi
	mov  $__bss_end,   %ecx
	sub  %edi, %ecx
	shr  $2, %ecx
	xor  %eax, %eax
	rep  stosl

	call setup_page_tables
	lgdt (gdt32_ptr)

	mov %cr4, %eax
	or  $0x20, %eax          /* PAE */
	mov %eax, %cr4

	mov $pml4, %eax
	mov %eax, %cr3

	mov $0xC0000080, %ecx    /* EFER */
	rdmsr
	or  $0x100, %eax         /* LME */
	wrmsr

	mov %cr0, %eax
	or  $0x80000001, %eax    /* PG + PE */
	mov %eax, %cr0

	ljmp $0x08, $long_mode_entry

setup_page_tables:
	mov $pdpt, %eax
	or  $0x3, %eax
	mov %eax, pml4
	mov $pd, %eax
	or  $0x3, %eax
	mov %eax, pdpt
	mov $0, %ecx
.fill_pd:
	mov %ecx, %eax
	shl $21, %eax
	or  $0x83, %eax          /* present + write + huge (2 MB) */
	mov %eax, pd(, %ecx, 8)
	inc %ecx
	cmp $512, %ecx
	jl  .fill_pd
	ret

.code64
long_mode_entry:
	mov $0x10, %ax
	mov %ax, %ds
	mov %ax, %es
	mov %ax, %ss
	xor %ax, %ax
	mov %ax, %fs
	mov %ax, %gs
	mov $__stack_top, %rsp
	movq $0, _dtb_ptr(%rip)
	call kernel_main
1:	hlt
	jmp 1b

.balign 8
gdt32:
	.quad 0
	.quad 0x00AF9A000000FFFF   /* 64-bit code */
	.quad 0x00CF92000000FFFF   /* data */
gdt32_end:

.balign 4
gdt32_ptr:
	.word gdt32_end - gdt32 - 1
	.long gdt32

.section ".bss"
.balign 4096
pml4: .skip 4096
pdpt: .skip 4096
pd:   .skip 4096

.balign 16
boot_stack32:     .skip 4096
boot_stack32_top:

.section ".data"
.balign 8
.global _dtb_ptr
_dtb_ptr: .quad 0
