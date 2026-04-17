ENTRY(_start)

SECTIONS
{
    . = 0x80000;

    .text : {
        KEEP(*(.text.boot))
        *(.text .text.* .gnu.linkonce.t*)
    }

    .vectors : {
        . = ALIGN(2048);
        *(.vectors)
    }

    .rodata : { *(.rodata .rodata.* .gnu.linkonce.r*) }

    PROVIDE(_data = .);
    .data : { *(.data .data.* .gnu.linkonce.d*) }

    .bss (NOLOAD) : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(.mbox_buf)
        *(COMMON)
        __bss_end = .;
    }

    _end = .;

    . = ALIGN(16);
    __stack_bottom = .;
    . += 0x10000;
    __stack_top = .;

    /DISCARD/ : { *(.comment) *(.gnu*) *(.note*) *(.eh_frame*) }
}

__bss_size = (__bss_end - __bss_start) >> 3;