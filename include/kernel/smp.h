#ifndef _KERNEL_SMP_H
#define _KERNEL_SMP_H

#include <kernel/hal.h>
#include <kernel/config.h>

/* Bring-up ceiling, separate from NR_CPUS (cpumask.h — the cpumask width). The
 * bulky per-CPU stacks are allocated to the detected count in smp_init(); what
 * stays static is sized by MAX_CPUS — all_cpus[], ap_boot_sync[], the x86
 * GDT/TSS tables, and the watchdog scan buffers. Sizing those by NR_CPUS (8192)
 * would cost MBs of BSS (cpu_tss alone ~850 KB) wasted on every machine; 256
 * exceeds any target Ranix runs on by 64x. Detection (FDT/MADT) is clamped to
 * it, so a larger box degrades gracefully rather than overflowing. cpumasks
 * still span the full NR_CPUS so affinity/IPI masks scale to a Linux-size box. */
#define MAX_CPUS CONFIG_MAX_CPUS

extern int smp_ncpus;
extern cpu_context_t *all_cpus[MAX_CPUS];

void smp_init(void);

/* Called on each secondary CPU after arch-level MMU and GS/tpidr setup */
void secondary_cpu_main(int cpu_id);

/* Send reschedule IPI to the given CPU (no-op if cpu_id == current cpu) */
void smp_ipi_resched(int cpu_id);

/* Run a full memory barrier on every other online CPU and wait for completion
 * (membarrier(2) MEMBARRIER_CMD_GLOBAL / *_EXPEDITED). The local barrier is the
 * caller's responsibility / implied by the IPI round-trip. */
void smp_membarrier(void);

#endif
