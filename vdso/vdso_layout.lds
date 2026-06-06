/* vDSO layout: force everything into ONE read-exec PT_LOAD at vaddr 0 with file
 * offset == virtual address, so the kernel can map the raw image contiguously
 * and libc's AT_SYSINFO_EHDR-relative symbol resolution lands correctly. A
 * default -shared link splits RX text from an RW .dynamic segment (page-aligned
 * apart), which breaks a flat copy. There are no relocations and no writable
 * globals, so a single RX segment is safe.
 *
 * The text segment carries FILEHDR + PHDRS so its p_vaddr is 0 (the ELF header
 * lands at byte 0 == AT_SYSINFO_EHDR). glibc derives the load bias as
 * AT_SYSINFO_EHDR minus the first PT_LOAD's p_vaddr; if the segment started past
 * the headers, that bias is off by the header size and _DYNAMIC resolves into
 * .dynstr — a fatal ld.so assertion. */
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
	text    PT_LOAD    FILEHDR PHDRS FLAGS(5);   /* R E — headers + whole image */
	dynamic PT_DYNAMIC FLAGS(4);
}
