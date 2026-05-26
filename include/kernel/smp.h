#ifndef _KERNEL_SMP_H
#define _KERNEL_SMP_H

#include <kernel/hal.h>

#define MAX_CPUS 4

extern int smp_ncpus;
extern cpu_context_t *all_cpus[MAX_CPUS];

void smp_init(void);

/* Called on each secondary CPU after arch-level MMU and GS/tpidr setup */
void secondary_cpu_main(int cpu_id);

/* Send reschedule IPI to the given CPU (no-op if cpu_id == current cpu) */
void smp_ipi_resched(int cpu_id);

#endif
