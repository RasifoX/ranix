#ifndef ASM_GDT_H
#define ASM_GDT_H

#include <uapi/types.h>

void gdt_init(void);

/* segment selectors */
#define GDT_NULL    0x00
#define GDT_KCODE   0x08
#define GDT_KDATA   0x10
#define GDT_UCODE   0x18   /* ring3 code */
#define GDT_UDATA   0x20   /* ring3 data */
#define GDT_TSS     0x28   /* 64-bit TSS (16-byte descriptor at index 5) */

void tss_set_rsp0(uint64_t rsp0);

#endif
