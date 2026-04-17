ENTRY(_start)

SECTIONS {
    . = 0x40080000;

    .vectors : {
        . = ALIGN(2048);
        *(.vectors)
    }

    .text : {
        *(.text.boot)
        *(.text)
    }

    .rodata : {
        *(.rodata)
    }

    .data : {
        *(.data)
    }

    . = ALIGN(8);
    __bss_start = .;
    .bss : {
        *(.bss)
    }
    . = ALIGN(8);
    __bss_end = .;

    . = ALIGN(16);
    __stack_bottom = .;
    . += 0x10000;        /* 64KB stack */
    __stack_top = .;
}
