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
	/* framebuffer request: type=5, flags=0 (optional), width/height/depth=0 (any) */
	.word 5; .word 0; .long 20; .long 0; .long 0; .long 32
	.balign 8
	.word 0; .word 0; .long 8
mb2_end:

.section ".text.boot"
.code32
.global _start
_start:
	cli
	mov  $boot_stack32_top, %esp

	/* Save Multiboot2 info pointer */
	mov  %ebx, _dtb_ptr

	/* zero .bss.boot (page tables, boot stack) */
	mov  $__bss_boot_start, %edi
	mov  $__bss_boot_end,   %ecx
	sub  %edi, %ecx
	shr  $2, %ecx
	xor  %eax, %eax
	rep  stosl

	call setup_page_tables
	lgdt (gdt32_ptr)

	mov  %cr4, %eax
	or   $0x20, %eax          /* PAE */
	mov  %eax, %cr4

	/* 5-level paging when the CPU has LA57 (CPUID.7.0:ECX[16]). pml5 reuses the
	 * existing pml4: pml5[0]/[504]/[511] all point to it, which reproduces the
	 * 48-bit-canonical kernel mappings at their 57-bit slots (identity@0,
	 * direct-map@504, kernel-text@511). User VA (<2^48) always uses pml5[0]. */
	mov  $7, %eax
	xor  %ecx, %ecx
	cpuid
	test $(1 << 16), %ecx
	jz   .four_level

	mov  $pml4, %eax
	or   $0x3, %eax
	mov  %eax, pml5
	mov  %eax, pml5 + 504*8
	mov  %eax, pml5 + 511*8

	mov  %cr4, %eax
	or   $(1 << 12), %eax     /* CR4.LA57 */
	mov  %eax, %cr4

	mov  $pml5, %eax
	mov  %eax, %cr3
	movb $1, la57_enabled
	jmp  .cr3_set
.four_level:
	mov  $pml4, %eax
	mov  %eax, %cr3
.cr3_set:

	/* EFER: LME (long mode) + NXE (no-execute) when CPUID advertises NX. NXE
	 * must be on before any NX-tagged PTE is walked, else bit 63 faults as
	 * reserved. ebx (multiboot ptr) was already saved to _dtb_ptr above. */
	mov  $0x80000001, %eax
	cpuid
	and  $(1 << 20), %edx     /* EDX.NX */
	mov  %edx, %ebx
	mov  $0xC0000080, %ecx
	rdmsr
	or   $0x100, %eax         /* LME */
	test %ebx, %ebx
	jz   1f
	or   $0x800, %eax         /* NXE */
1:
	wrmsr

	mov  %cr0, %eax
	or   $0x80000001, %eax    /* PG + PE */
	mov  %eax, %cr0

	ljmp $0x08, $long_mode_entry

setup_page_tables:
	/* identity: PML4[0] → pdpt → pd (512 × 2MB, 0..1GB) */
	mov  $pdpt, %eax
	or   $0x3, %eax
	mov  %eax, pml4

	mov  $pd, %eax
	or   $0x3, %eax
	mov  %eax, pdpt

	/* high half: PML4[511] → pdpt_hh[510] → pd (kernel text at -2GB) */
	mov  $pdpt_hh, %eax
	or   $0x3, %eax
	mov  %eax, pml4 + 511*8

	mov  $pd, %eax
	or   $0x3, %eax
	mov  %eax, pdpt_hh + 510*8

	/* direct map: PML4[256] → pdpt_dm, 1GB huge pages (PDPE1GB, no PD needed).
	 * Fill all 512 entries → 512 GB physical (matches aarch64's range); the
	 * PMM only manages RAM the memory map reports, so unbacked entries are
	 * just page-table slots that are never touched. */
	mov  $pdpt_dm, %eax
	or   $0x3, %eax
	mov  %eax, pml4 + 256*8

	mov  $0, %ecx
.fill_dm:
	mov  %ecx, %eax            /* low dword: ((i & 3) << 30) | 0x83 */
	and  $3, %eax
	shl  $30, %eax
	or   $0x83, %eax
	mov  %eax, pdpt_dm(, %ecx, 8)
	mov  %ecx, %eax            /* high dword: i >> 2 (bits above 4 GB) */
	shr  $2, %eax
	mov  %eax, pdpt_dm + 4(, %ecx, 8)
	inc  %ecx
	cmp  $512, %ecx
	jl   .fill_dm

	/* fill pd: 0..1GB in 2MB pages (kernel text at -2GB uses pdpt_hh → pd) */
	mov  $0, %ecx
.fill_pd:
	mov  %ecx, %eax
	shl  $21, %eax
	or   $0x83, %eax
	mov  %eax, pd(, %ecx, 8)
	inc  %ecx
	cmp  $512, %ecx
	jl   .fill_pd

	ret

.code64
long_mode_entry:
	mov  $0x10, %ax
	mov  %ax, %ds
	mov  %ax, %es
	mov  %ax, %ss
	xor  %ax, %ax
	mov  %ax, %fs
	mov  %ax, %gs
	movabsq $long_mode_high, %rax
	jmp  *%rax

.section ".text"
.code64
.global long_mode_high
long_mode_high:
	movabsq $__stack_top, %rsp

	/* clear high .bss */
	movabsq $__bss_start, %rdi
	movabsq $__bss_end,   %rcx
	sub  %rdi, %rcx
	shr  $3, %rcx
	xor  %eax, %eax
	rep  stosq

	call kernel_main
1:	hlt
	jmp  1b

.section ".bss.boot"
.balign 4096
.global pml5
pml5:    .skip 4096
.global pml4
pml4:    .skip 4096
pdpt:    .skip 4096
pd:      .skip 4096
pdpt_hh: .skip 4096
.global pdpt_dm
pdpt_dm: .skip 4096

.balign 16
boot_stack32:     .skip 4096
boot_stack32_top:

.global la57_enabled
la57_enabled: .skip 8

.section ".data.boot", "aw"
.balign 8
.global _dtb_ptr
_dtb_ptr: .quad 0

.global gdt32
gdt32:
	.quad 0
	.quad 0x00AF9A000000FFFF   /* 64-bit code */
	.quad 0x00CF92000000FFFF   /* data */
gdt32_end:

.balign 4
gdt32_ptr:
	.word gdt32_end - gdt32 - 1
	.long gdt32

.section ".data"
.balign 8
