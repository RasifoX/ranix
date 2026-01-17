#include "gdt.h"
#include "string.h"

gdt_entry_t gdt_entries[6];
gdt_ptr_t gdt_ptr;
tss_entry_t tss_entry;

extern void gdt_flush(uint32_t);
extern void tss_flush(void);

static void gdt_set_gate(int32_t num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt_entries[num].base_low = (base & 0xFFFF);
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].base_high = (base >> 24) & 0xFF;

    gdt_entries[num].limit_low = (limit & 0xFFFF);
    gdt_entries[num].granularity = (limit >> 16) & 0x0F;

    gdt_entries[num].granularity |= (gran & 0xF0);
    gdt_entries[num].access = access;
}

static void write_tss(int32_t num, uint16_t ss0, uint32_t esp0)
{
    uint32_t base = (uint32_t)&tss_entry;
    uint32_t limit = sizeof(tss_entry);

    gdt_set_gate(num, base, limit, 0x89, 0x00);

    memset(&tss_entry, 0, sizeof(tss_entry));

    tss_entry.ss0 = ss0;
    tss_entry.esp0 = esp0;
    
    tss_entry.cs = 0x08 | 0;
    tss_entry.ss = 0x10 | 0;
    tss_entry.ds = 0x10 | 0;
    tss_entry.es = 0x10 | 0;
    tss_entry.fs = 0x10 | 0;
    tss_entry.gs = 0x10 | 0;

    tss_entry.iomap_base = sizeof(tss_entry);
}

void init_gdt()
{
    gdt_ptr.limit = (sizeof(gdt_entry_t) * 6) - 1;
    gdt_ptr.base = (uint32_t)&gdt_entries;

    gdt_set_gate(0, 0, 0, 0, 0);                // 0: Null
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // 1: Kernel Code
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // 2: Kernel Data
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // 3: User Code
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // 4: User Data

    write_tss(5, 0x10, 0);

    gdt_flush((uint32_t)&gdt_ptr);

    tss_flush();
}

void tss_set_stack(uint32_t kss, uint32_t kesp)
{
    tss_entry.ss0 = kss;
    tss_entry.esp0 = kesp;
}