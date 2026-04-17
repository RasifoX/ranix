#ifndef ASM_CONTEXT_H
#define ASM_CONTEXT_H

#include <uapi/types.h>

/*
 * Saved on the stack by exceptions_asm.S before calling C handlers.
 * Hardware (CPU) pushes ss/rsp/rflags/cs/rip automatically.
 * We push the rest plus a vector number and error code slot.
 *
 *   offset 0   : r15-r8, rbp, rdi, rsi, rdx, rcx, rbx, rax  (15 * 8)
 *   offset 120 : vector
 *   offset 128 : error_code
 *   offset 136 : rip, cs, rflags, rsp, ss  (CPU-pushed, 5 * 8)
 *   total      : 176 bytes
 */
typedef struct {
	uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
	uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
	uint64_t vector;
	uint64_t error_code;
	uint64_t rip, cs, rflags, rsp, ss;
} arch_context_t;

#define ARCH_SPSR_KERNEL_TASK  0x202UL  /* RFLAGS: IF=1, reserved bit 1 */

#endif
