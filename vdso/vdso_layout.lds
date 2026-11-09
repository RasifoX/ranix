/*
 * vDSO image layout (x86_64 and aarch64).
 *
 * The kernel copies this image into every process as one flat block and passes
 * its start as AT_SYSINFO_EHDR. For that to work:
 *
 *  - There must be exactly one PT_LOAD, read+execute, starting at virtual
 *    address 0 with file offset == virtual address. A plain -shared link would
 *    put .dynamic in a second, writable, page-aligned segment, and a flat copy
 *    would then be wrong.
 *  - That PT_LOAD must also cover the ELF and program headers, so its p_vaddr
 *    is 0. The C library computes the load bias as AT_SYSINFO_EHDR minus the
 *    first PT_LOAD's p_vaddr; if the segment began after the headers, _DYNAMIC
 *    would resolve into .dynstr and ld.so aborts.
 *
 * The image has no relocations and no writable data, so a single read-execute
 * segment is enough. Input sections not listed here are not expected; any
 * .data/.bss that does appear is folded into the same segment.
 */

PHDRS
{
	image   PT_LOAD    FILEHDR PHDRS FLAGS(5);  /* PF_R | PF_X */
	dyninfo PT_DYNAMIC FLAGS(4);                /* PF_R */
}

SECTIONS
{
	/* Headers first, then the tables the dynamic linker reads to look up
	 * symbols, in the order the toolchain's own default script uses. */
	. = SIZEOF_HEADERS;

	.hash           : { *(.hash) }           :image
	.gnu.hash       : { *(.gnu.hash) }
	.dynsym         : { *(.dynsym) }
	.dynstr         : { *(.dynstr) }
	.gnu.version    : { *(.gnu.version) }
	.gnu.version_d  : { *(.gnu.version_d) }
	.gnu.version_r  : { *(.gnu.version_r) }

	/* .dynamic sits in the load segment and is also named by PT_DYNAMIC. */
	.dynamic        : { *(.dynamic) }        :image :dyninfo

	/* Constants and code; everything stays in the one segment. */
	.rodata         : { *(.rodata*) }        :image
	.text           : { *(.text*) }
	.data           : { *(.data* .got* .got.plt* .bss*) }

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame_hdr)
		*(.eh_frame*)
	}
}
