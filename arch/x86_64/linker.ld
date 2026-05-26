OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHYS_BASE   = 0x100000;
VIRT_BASE   = 0xFFFFFFFF80100000;
VIRT_OFFSET = 0xFFFFFFFF80000000;

SECTIONS
{
    . = PHYS_BASE;

    .multiboot2 : {
        KEEP(*(.multiboot2))
    }

    .text.boot : {
        *(.text.boot)
    }

    /* AP trampoline — kept in low physical memory so APs can jump to it */
    .trampoline : {
        KEEP(*(.trampoline))
    }

    .data.boot : {
        . = ALIGN(8);
        *(.data.boot)
    }

    .bss.boot (NOLOAD) : {
        . = ALIGN(4096);
        __bss_boot_start = .;
        *(.bss.boot)
        . = ALIGN(4096);
        __bss_boot_end = .;
    }

    . = ALIGN(16);
    __kernel_phys_end_boot = .;

    . = VIRT_BASE + (__kernel_phys_end_boot - PHYS_BASE);

    .text : AT(ADDR(.text) - VIRT_OFFSET) {
        *(.text*)
    }

    .rodata : AT(ADDR(.rodata) - VIRT_OFFSET) {
        *(.rodata*)
    }

    .data : AT(ADDR(.data) - VIRT_OFFSET) {
        *(.data*)
    }

    . = ALIGN(4096);
    __bss_start = .;
    .bss (NOLOAD) : {
        *(.bss*)
        *(COMMON)
    }
    . = ALIGN(8);
    __bss_end = .;

    . = ALIGN(16);
    . += 16384;
    __stack_top = .;

    __kernel_phys_end = __stack_top - VIRT_OFFSET;
}
