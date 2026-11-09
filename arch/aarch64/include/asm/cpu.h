#ifndef ASM_CPU_H
#define ASM_CPU_H

/* aarch64 arch-internal interfaces: exception vectors, secondary-CPU
 * bring-up. */

#include <uapi/types.h>

void exception_vectors(void);                    /* exceptions_asm.S */
void secondary_trampoline(void);                 /* boot.S */

#endif
