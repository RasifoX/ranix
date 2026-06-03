/* vDSO layout: force everything into ONE read-exec PT_LOAD at vaddr 0 with file
 * offset == virtual address, so the kernel can map the raw image contiguously
 * and libc's AT_SYSINFO_EHDR-relative symbol resolution lands correctly. A
 * default -shared link splits RX text from an RW .dynamic segment (page-aligned
 * apart), which breaks a flat copy. There are no relocations and no writable
 * globals, so a single RX segment is safe. */
SECTIONS
{
	. = SIZEOF_HEADERS;

	.hash          : { *(.hash) }
	.gnu.hash      : { *(.gnu.hash) }
	.dynsym        : { *(.dynsym) }
	.dynstr        : { *(.dynstr) }
	.gnu.version   : { *(.gnu.version) }
	.gnu.version_d : { *(.gnu.version_d) }
	.gnu.version_r : { *(.gnu.version_r) }
	.dynamic       : { *(.dynamic) }            :text :dynamic
	.rodata        : { *(.rodata*) }            :text
	.text          : { *(.text*) }              :text
	.data          : { *(.data* .got* .got.plt* .bss*) } :text

	/DISCARD/ : { *(.note*) *(.eh_frame*) *(.eh_frame_hdr) *(.comment) }
}

PHDRS
{
	text    PT_LOAD    FLAGS(5);   /* R E — the whole image */
	dynamic PT_DYNAMIC FLAGS(4);
}
