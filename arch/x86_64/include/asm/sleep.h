/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef ASM_SLEEP_H
#define ASM_SLEEP_H

/* One CPU's state across S3, shared by arch/x86_64/sleep.c and wakeup.S. The
 * general registers are only the callee-saved ones: sleep_save_cpu is called
 * like setjmp, and comes back a second time (returning 1) from the wake page. */

#define SC_RBX     0x00
#define SC_RBP     0x08
#define SC_R12     0x10
#define SC_R13     0x18
#define SC_R14     0x20
#define SC_R15     0x28
#define SC_RSP     0x30
#define SC_RIP     0x38
#define SC_RFLAGS  0x40
#define SC_CR0     0x48
#define SC_CR3     0x50
#define SC_CR4     0x58
#define SC_EFER    0x60
#define SC_FSBASE  0x68
#define SC_GSBASE  0x70
#define SC_KGSBASE 0x78
#define SC_PAT     0x80
#define SC_XCR0    0x88     /* 0: no XSAVE, the FPU went by FXSAVE */
#define SC_GDTR    0x90     /* 10 bytes */
#define SC_IDTR    0xA0     /* 10 bytes */
#define SC_FPU     0x100    /* 64-byte aligned, XSAVE or FXSAVE image */
#define SC_FPU_LEN 4096
#define SC_SIZE    (SC_FPU + SC_FPU_LEN)

/* The wake page: real mode entry for the firmware (FACS waking vector) and for
 * an AP's SIPI, then protected and long mode as trampoline.S does, then
 * sleep_resume_entry(ctx) in the kernel's high half. */
#define WAKE_PHYS      0x8000
#define WAKE_DATA      0xF00
#define WD_TOP         0x00     /* PML4 or PML5, physical */
#define WD_CTX         0x08     /* sleep_cpu_t *, virtual */
#define WD_ENTRY       0x10     /* sleep_resume_entry, virtual */

#ifndef __ASSEMBLER__
#include <uapi/types.h>
typedef struct { uint8_t b[SC_SIZE]; } __attribute__((aligned(64))) sleep_cpu_t;

int  sleep_save_cpu(sleep_cpu_t *c);     /* 0 now; 1 when the wake page brings the CPU back */
void sleep_resume_entry(void);           /* wakeup.S: rdi = ctx, never returns */
extern char sleep_wake_start[], sleep_wake_end[];

/* smp.c */
void lapic_timer_mask(void);
void lapic_sleep_arm(uint64_t ns);
void lapic_resume_cpu(void);
void lapic_timer_start(void);
void arch_smp_wake_cpu(int cpu_id, uint64_t page_phys);
void arch_smp_park_send(int cpu_id);
#endif

#endif
