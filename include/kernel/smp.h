#ifndef _KERNEL_SMP_H
#define _KERNEL_SMP_H

#include <kernel/hal.h>

/* Ceiling for the statically-sized per-CPU arrays. The bulky per-CPU stacks are
 * allocated dynamically to the detected count in smp_init(); what remains static
 * is sized by MAX_CPUS — all_cpus[], ap_boot_sync[], the x86 GDT/TSS tables, and
 * the watchdog scan buffers. At 8192 those cost ~2 MB of BSS (the x86 GDT/TSS
 * alone is ~1.3 MB), wasted on every machine. 256 still exceeds any target Ranix
 * runs on by 64x; detection (FDT/MADT) is clamped to it, so a larger box degrades
 * gracefully rather than overflowing. */
#define MAX_CPUS 256

extern int smp_ncpus;
extern cpu_context_t *all_cpus[MAX_CPUS];

void smp_init(void);

/* Called on each secondary CPU after arch-level MMU and GS/tpidr setup */
void secondary_cpu_main(int cpu_id);

/* Send reschedule IPI to the given CPU (no-op if cpu_id == current cpu) */
void smp_ipi_resched(int cpu_id);

#endif
