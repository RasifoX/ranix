#ifndef _KERNEL_SMP_H
#define _KERNEL_SMP_H

#include <kernel/hal.h>

/* Ceiling for the statically-sized arrays (all_cpus[], ap_boot_sync[]); the
 * bulky per-CPU memory is allocated dynamically to the detected count in
 * smp_init(), so this stays cheap (~50 KB) to keep high. A single buddy alloc
 * caps kstacks near ~2048 CPUs; beyond that needs vmalloc (follow-up). */
#define MAX_CPUS 1024

extern int smp_ncpus;
extern cpu_context_t *all_cpus[MAX_CPUS];

void smp_init(void);

/* Called on each secondary CPU after arch-level MMU and GS/tpidr setup */
void secondary_cpu_main(int cpu_id);

/* Send reschedule IPI to the given CPU (no-op if cpu_id == current cpu) */
void smp_ipi_resched(int cpu_id);

#endif
