/*
 * Raspberry Pi 4 (BCM2711) kernel image layout.
 *
 * The firmware loads kernel8.img at physical 0x80000 and jumps to it with the
 * MMU off. The first part (boot code, its data, the early page tables and the
 * boot stack) is linked at that physical address. Everything after it is
 * linked in the higher half, VIRT_OFFSET above where it is loaded, and boot.S
 * switches to those addresses once the MMU is on.
 *
 * Symbols the C and assembly code use:
 *   __stack_top                         boot.S, initial stack
 *   __bss_start, __bss_end              boot.S, zeroed before C runs
 *   __start___ex_table, __stop___ex_table   uaccess fault fixups
 *   __kernel_phys_end                   kernel/main.c, first free physical page
 */

ENTRY(_start)

LOAD_ADDR   = 0x80000;
VIRT_OFFSET = 0xFFFF800000000000;

SECTIONS
{
    /* ---- identity-mapped part, runs before the MMU is enabled ---- */
    . = LOAD_ADDR;

    .text.boot : {
        KEEP(*boot.o(.text.boot))       /* _start must be the first byte */
        KEEP(*(.text.boot))
    }

    .data.boot : {
        . = ALIGN(8);
        *(.data.boot)
    }

    .bss.boot (NOLOAD) : {              /* early page tables, page aligned */
        . = ALIGN(4096);
        *(.bss.boot)
        . = ALIGN(4096);
    }

    . = ALIGN(16);
    __stack_bottom = .;
    . = . + 64K;                        /* boot stack */
    __stack_top = .;
    __phys_end = .;

    /* ---- higher half: same load order, virtual addresses ---- */
    . = __phys_end + VIRT_OFFSET;

    .text : AT(ADDR(.text) - VIRT_OFFSET) {
        *(.text .text.*)
    }

    .vectors : AT(ADDR(.vectors) - VIRT_OFFSET) {
        . = ALIGN(2048);                /* VBAR_EL1 needs 2 KiB alignment */
        *(.vectors)
    }

    .rodata : AT(ADDR(.rodata) - VIRT_OFFSET) {
        *(.rodata .rodata.*)
    }

    . = ALIGN(8);
    __ex_table : AT(ADDR(__ex_table) - VIRT_OFFSET) {
        __start___ex_table = .;
        *(.__ex_table)
        __stop___ex_table = .;
    }

    .data : AT(ADDR(.data) - VIRT_OFFSET) {
        *(.data .data.*)
    }

    .bss (NOLOAD) : {
        . = ALIGN(16);
        __bss_start = .;
        *(.bss .bss.*)
        *(.mbox_buf)                    /* mailbox buffer, see bcm2711_mbox.c */
        *(COMMON)
        __bss_end = .;
    }

    _end = .;
    __kernel_phys_end = _end - VIRT_OFFSET;

    /* Not needed in a bare-metal image. */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.gnu*)
    }
}
