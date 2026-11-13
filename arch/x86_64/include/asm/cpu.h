/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
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
void acpi_button_start(void);   /* acpi_events.c: power button, and the SCI with ranix.sci=1 */
typedef struct {
	uint16_t pm1a_evt, pm1a_cnt, sci_int;
	uint8_t  pm1_len, gpe0_len, acpi_enable;
	uint32_t gpe0_blk, smi_cmd, dsdt_len;
	const uint8_t *dsdt;
} acpi_ev_info_t;
int  acpi_events_info(acpi_ev_info_t *o);   /* acpi.c: from the FADT; -1 without PM1 */
typedef struct {
	uint8_t       *facs;                /* firmware_waking_vector at +12, X_ at +24 */
	uint16_t       pm1a_cnt, pm1b_cnt, pm1a_evt;
	uint8_t        pm1_len;
	uint32_t       fadt_flags;          /* bit 21: LOW_POWER_S0_IDLE_CAPABLE */
	const uint8_t *dsdt;
	uint32_t       dsdt_len;
} acpi_sleep_info_t;
int  acpi_sleep_info(acpi_sleep_info_t *o);  /* acpi.c: for S3; -1 without PM1 */
int  acpi_sleep_wake_check(int swallow);     /* acpi_events.c: a press or an SCI event seen while asleep */
int  acpi_sci_irq(void);                     /* acpi_events.c: the SCI line when it is taken, else -1 */
void hpet_resume(void);
void acpi_poweroff(void);
void acpi_reset(void);

/* acpi.c: clock sources */
extern uint64_t tsc_mult;
void     tsc_calibrate(void);
uint64_t tsc_now_ns(void);
uint64_t tsc_ns_at(uint64_t tsc);   /* the clock at a TSC value already read */
uint64_t hpet_now_ns(void);

/* exceptions.c */
void prof_init(void);

#endif
