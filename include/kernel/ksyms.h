#ifndef KERNEL_KSYMS_H
#define KERNEL_KSYMS_H

#include <uapi/types.h>

/* Resolve a kernel text address to "name+offset". Returns the symbol name and
 * sets *off to the byte offset into it, or NULL if no symbol covers addr. */
const char *ksym_lookup(uint64_t addr, uint64_t *off);

extern const char ranix_version[];
extern const char ranix_gitrev[];
extern const char ranix_builddate[];
extern const char ranix_arch[];

#endif
