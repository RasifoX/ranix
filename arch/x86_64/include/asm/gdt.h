#ifndef ASM_GDT_H
#define ASM_GDT_H

void gdt_init(void);

/* segment selectors */
#define GDT_NULL    0x00
#define GDT_KCODE   0x08
#define GDT_KDATA   0x10

#endif
