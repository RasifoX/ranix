#ifndef ASM_CPU_H
#define ASM_CPU_H

/* x86_64 arch-internal interfaces: CPU setup, local APIC, AP bring-up,
 * multiboot2, ACPI and the x86 clock sources. Generic code includes this only
 * for calls it already makes under #if defined(__x86_64__). */

#include <uapi/types.h>
#include <kernel/fb.h>

/* cpu.c */
extern uint8_t avx_on;
void syscall_entry(void);                        /* exceptions_asm.S */
void arch_syscall_init(void);
void arch_pat_init(void);
void arch_hwp_init(void);
void arch_idle_init(void);

/* smp.c: local APIC and AP bring-up */
extern uint32_t cpu_apic_id[];
extern char trampoline_start[], trampoline_end[];   /* trampoline.S */
uint32_t lapic_self_id(void);
void lapic_eoi(void);
void lapic_bsp_tick_takeover(void);
void arch_tlb_shootdown_all(void);
void idt_load(void);                             /* idt.c */

/* multiboot2.c */
int  mb2_get_fb(fb_info_t *out);
void mb2_foreach_mem_region(void (*cb)(uint64_t, uint64_t, void *), void *ud);
void mb2_get_reserved(uint64_t *base, uint32_t *size);

/* acpi.c */
int  acpi_detect_cpus(void);
void acpi_button_start(void);
void acpi_poweroff(void);
void acpi_reset(void);

/* acpi.c: clock sources */
extern uint64_t tsc_mult;
void     tsc_calibrate(void);
uint64_t tsc_now_ns(void);
uint64_t hpet_now_ns(void);

/* exceptions.c */
void prof_init(void);

#endif
