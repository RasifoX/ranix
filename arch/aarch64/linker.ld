ENTRY(_start)

PHYS_BASE   = 0x40080000;
VIRT_BASE   = 0xFFFF800040080000;
VIRT_OFFSET = 0xFFFF800000000000;

SECTIONS {
    . = PHYS_BASE;

    .text.boot : {
        KEEP(*boot.o(.text.boot))
        KEEP(*(.text.boot))
    }

    .data.boot : {
        . = ALIGN(8);
        *(.data.boot)
    }

    .bss.boot (NOLOAD) : {
        . = ALIGN(4096);
        *(.bss.boot)
        . = ALIGN(4096);
    }

    . = ALIGN(16);
    __stack_bottom = .;
    . += 0x10000;
    __stack_top = .;

    __phys_end = .;

    . = VIRT_BASE + (__phys_end - PHYS_BASE);

    . = ALIGN(2048);
    .vectors : AT(ADDR(.vectors) - VIRT_OFFSET) {
        *(.vectors)
    }

    .text : AT(ADDR(.text) - VIRT_OFFSET) {
        *(.text)
    }

    .rodata : AT(ADDR(.rodata) - VIRT_OFFSET) {
        *(.rodata)
    }

    . = ALIGN(8);
    __ex_table : AT(ADDR(__ex_table) - VIRT_OFFSET) {
        __start___ex_table = .;
        *(.__ex_table)
        __stop___ex_table = .;
    }

    .data : AT(ADDR(.data) - VIRT_OFFSET) {
        *(.data)
    }

    . = ALIGN(8);
    __bss_start = .;
    .bss (NOLOAD) : {
        *(.bss)
    }
    . = ALIGN(8);
    __bss_end = .;

    __kernel_phys_end = __bss_end - VIRT_OFFSET;
}
