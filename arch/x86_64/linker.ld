OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .text : {
        KEEP(*(.multiboot2))
        *(.text.boot)
        *(.text*)
    }

    .rodata : { *(.rodata*) }

    .data : { *(.data*) }

    . = ALIGN(4096);
    .bss : {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(16);
    . += 16384;
    __stack_top = .;
}
