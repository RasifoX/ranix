#ifndef KERNEL_EXTABLE_H
#define KERNEL_EXTABLE_H

#include <uapi/types.h>

/* Exception-fixup table: a kernel access annotated with _ASM_EXTABLE recovers
 * from a fault by branching to its fixup instead of panicking. Entries hold
 * absolute addresses (the kernel is linked at a fixed VA, never relocated). */
struct ex_entry {
	unsigned long insn;
	unsigned long fixup;
};

#define _ASM_EXTABLE(insn, fixup)            \
	"  .pushsection .__ex_table, \"a\"\n"    \
	"  .balign 8\n"                          \
	"  .quad " insn "\n"                     \
	"  .quad " fixup "\n"                    \
	"  .popsection\n"

unsigned long search_extable(unsigned long pc);

#endif
