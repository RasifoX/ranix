#ifndef _KERNEL_SMP_H
#define _KERNEL_SMP_H

#include <kernel/hal.h>

/* Ceiling for the statically-sized arrays (all_cpus[] pointers + .data.boot
 * ap_boot_sync[]); the bulky per-CPU memory is allocated dynamically to the
 * detected count in smp_init(), each stack as its own block — so no single
 * allocation caps the core count. ~330 KB static at 8192 (Linux CONFIG_MAXSMP). */
#define MAX_CPUS 8192

extern int smp_ncpus;
extern cpu_context_t *all_cpus[MAX_CPUS];

void smp_init(void);

/* Called on each secondary CPU after arch-level MMU and GS/tpidr setup */
void secondary_cpu_main(int cpu_id);

/* Send reschedule IPI to the given CPU (no-op if cpu_id == current cpu) */
void smp_ipi_resched(int cpu_id);

#endif
