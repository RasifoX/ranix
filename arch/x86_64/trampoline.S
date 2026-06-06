/*
 * x86_64 AP trampoline — copied to physical 0x7000 at runtime.
 * APs start in 16-bit real mode here.
 * Data area at page-relative offset 0xF00.
 *
 * Data layout (at TRAMP_PHYS + DATA_OFF):
 *   +0x00  uint64_t pml4_phys
 *   +0x08  uint64_t sp_virt      (virtual stack top for C code)
 *   +0x10  uint32_t cpu_id
 *   +0x14  uint32_t _pad[3]
 *   +0x20  GDT entries (5 × 8 = 40 bytes)
 *   +0x50  GDT pointer (6 bytes: limit + base)
 */

#define TRAMP_PHYS   0x7000
#define DATA_OFF     0xF00

#define TDATA_PML4   0x00
#define TDATA_SP     0x08
#define TDATA_CPU_ID 0x10

.section ".trampoline", "ax"
.code16
.global trampoline_start
trampoline_start:
	cli
	cld

	xor  %ax, %ax
	mov  %ax, %ds
	mov  %ax, %es
	mov  %ax, %ss

	/* Enable A20 */
	inb  $0x92, %al
	or   $0x02, %al
	outb %al, $0x92

	/* Load minimal 32/64-bit GDT */
	lgdtl (TRAMP_PHYS + DATA_OFF + 0x50)

	/* Enter protected mode */
	mov  %cr0, %eax
	or   $0x01, %eax
	mov  %eax, %cr0

	ljmp $0x08, $(TRAMP_PHYS + .Lpm32 - trampoline_start)

.code32
.Lpm32:
	mov  $0x10, %ax
	mov  %ax, %ds
	mov  %ax, %es
	mov  %ax, %ss
	xor  %ax, %ax
	mov  %ax, %fs
	mov  %ax, %gs

	/* Enable PAE */
	mov  %cr4, %eax
	or   $0x20, %eax
	mov  %eax, %cr4

	/* 5-level paging if the CPU has LA57 (the BSP enabled it the same way);
	 * setup_trampoline() then hands us a PML5 in TDATA_PML4. */
	mov  $7, %eax
	xor  %ecx, %ecx
	cpuid
	test $(1 << 16), %ecx
	jz   .Lno_la57
	mov  %cr4, %eax
	or   $(1 << 12), %eax     /* CR4.LA57 */
	mov  %eax, %cr4
.Lno_la57:

	/* Detect NX (CPUID.80000001:EDX.NX) into edi before we touch cr3/eax. */
	mov  $0x80000001, %eax
	cpuid
	and  $(1 << 20), %edx
	mov  %edx, %edi

	/* Load top-level table (PML5 under 5-level, else PML4) */
	mov  $(TRAMP_PHYS + DATA_OFF + TDATA_PML4), %ebx
	mov  (%ebx), %eax
	mov  %eax, %cr3

	/* Enable LME + NXE (NXE before any NX-tagged PTE is walked). */
	mov  $0xC0000080, %ecx
	rdmsr
	or   $0x100, %eax
	test %edi, %edi
	jz   1f
	or   $0x800, %eax
1:
	wrmsr

	/* Enable paging */
	mov  %cr0, %eax
	or   $0x80000001, %eax
	mov  %eax, %cr0

	ljmp $0x18, $(TRAMP_PHYS + .Llm64 - trampoline_start)

.code64
.Llm64:
	mov  $0x10, %ax
	mov  %ax, %ds
	mov  %ax, %es
	mov  %ax, %ss
	xor  %ax, %ax
	mov  %ax, %fs
	mov  %ax, %gs

	/* Load virtual stack */
	movabsq $(TRAMP_PHYS + DATA_OFF + TDATA_SP), %rax
	mov  (%rax), %rsp

	/* cpu_id argument */
	movabsq $(TRAMP_PHYS + DATA_OFF + TDATA_CPU_ID), %rax
	movl (%rax), %edi

	/* Call C entry — synchronization happens inside secondary_cpu_main */
	movabsq $secondary_cpu_main, %rax
	call *%rax

.Lhalt:
	hlt
	jmp  .Lhalt

/* Data area at DATA_OFF */
.org DATA_OFF
trampoline_data:
	.quad 0          /* pml4_phys */
	.quad 0          /* sp_virt */
	.long 0          /* cpu_id */
	.long 0          /* _pad */

/* GDT at DATA_OFF + 0x20 */
.org (DATA_OFF + 0x20)
trampoline_gdt:
	.quad 0                       /* null */
	.quad 0x00CF9A000000FFFF      /* 32-bit code */
	.quad 0x00CF92000000FFFF      /* 32-bit data */
	.quad 0x00AF9A000000FFFF      /* 64-bit code */
	.quad 0x00AF92000000FFFF      /* 64-bit data */
trampoline_gdt_end:

/* GDT pointer at DATA_OFF + 0x50 */
.org (DATA_OFF + 0x50)
trampoline_gdt_ptr:
	.word (trampoline_gdt_end - trampoline_gdt - 1)
	.long (TRAMP_PHYS + DATA_OFF + 0x20)

.global trampoline_end
trampoline_end:
