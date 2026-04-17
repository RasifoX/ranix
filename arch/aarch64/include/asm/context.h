#ifndef ASM_CONTEXT_H
#define ASM_CONTEXT_H

#include <uapi/types.h>

/*
 * offsets used by exceptions_asm.S (hardcoded):
 *   0   x0-x30
 *   248 sp
 *   256 elr
 *   264 spsr
 */
typedef struct {
	uint64_t regs[31];
	uint64_t sp;
	uint64_t elr;
	uint64_t spsr;
} arch_context_t;

#define ARCH_SPSR_KERNEL_TASK  0x00000005UL  /* EL1h, DAIF=0 */

#endif
