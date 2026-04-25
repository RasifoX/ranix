ENTRY(_start)

PHYS_BASE   = 0x80000;
VIRT_BASE   = 0xFFFFFF8000080000;
VIRT_OFFSET = 0xFFFFFF8000000000;

SECTIONS
{
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

    .text : AT(ADDR(.text) - VIRT_OFFSET) {
        *(.text .text.* .gnu.linkonce.t*)
    }

    .vectors : AT(ADDR(.vectors) - VIRT_OFFSET) {
        . = ALIGN(2048);
        *(.vectors)
    }

    .rodata : AT(ADDR(.rodata) - VIRT_OFFSET) {
        *(.rodata .rodata.* .gnu.linkonce.r*)
    }

    PROVIDE(_data = .);
    .data : AT(ADDR(.data) - VIRT_OFFSET) {
        *(.data .data.* .gnu.linkonce.d*)
    }

    .bss (NOLOAD) : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(.mbox_buf)
        *(COMMON)
        __bss_end = .;
    }

    _end = .;
    __kernel_phys_end = _end - VIRT_OFFSET;

    /DISCARD/ : { *(.comment) *(.gnu*) *(.note*) *(.eh_frame*) }
}

__bss_size = (__bss_end - __bss_start) >> 3;
