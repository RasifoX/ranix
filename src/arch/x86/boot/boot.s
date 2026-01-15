.set MAGIC,      0xE85250D6
.set ARCH,       0
.set HEADER_LEN, multiboot_header_end - multiboot_header_start
.set CHECKSUM,   -(MAGIC + ARCH + HEADER_LEN)

.section .multiboot
.align 8
multiboot_header_start:
    .long MAGIC
    .long ARCH
    .long HEADER_LEN
    .long CHECKSUM

    .short 0
    .short 0
    .long 8
multiboot_header_end:

.section .bss
.align 16
stack_bottom:
.skip 16384
stack_top:

.section .text
.global _start
.type _start, @function

_start:
    mov $stack_top, %esp

    push %ebx
    push %eax

    call kernel_main

    cli
1:  hlt
    jmp 1b