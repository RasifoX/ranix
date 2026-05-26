#include <kernel/hal.h>
#include <kernel/smp.h>
#include <kernel/kprintf.h>
#include <kernel/board.h>

#define PSCI_CPU_ON  0xC4000003UL
#define GICD_SGIR    0xF00

void arch_smp_boot_cpu(int cpu_id)
{
	extern void secondary_trampoline(void);
	uint64_t entry_phys = (uint64_t)secondary_trampoline;
	uint64_t mpidr      = (uint64_t)cpu_id;
	uint64_t context_id = (uint64_t)cpu_id;

	register uint64_t r0 __asm__("x0") = PSCI_CPU_ON;
	register uint64_t r1 __asm__("x1") = mpidr;
	register uint64_t r2 __asm__("x2") = entry_phys;
	register uint64_t r3 __asm__("x3") = context_id;
	__asm__ volatile("hvc #0" :: "r"(r0), "r"(r1), "r"(r2), "r"(r3) : "memory");
}

void arch_secondary_init(int cpu_id)
{
	/* all_cpus[cpu_id] is already set by secondary_cpu_main */
	cpu_context_t *cpu = all_cpus[cpu_id];
	__asm__ volatile("msr tpidr_el1, %0" :: "r"(cpu));

	/* Install exception vector table */
	extern void exceptions_init(void);
	exceptions_init();

	uint64_t cpacr;
	__asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
	cpacr |= (0x3ULL << 20);
	__asm__ volatile("msr cpacr_el1, %0" :: "r"(cpacr));
	__asm__ volatile("isb");
}

void arch_smp_ipi_send(int cpu_id)
{
	board_config_t *cfg = board_get_config();
	uint64_t gicd = cfg->gic_dist_base;
	if (!gicd) return;
	/* dsb ish: ensure all preceding stores (rq_head etc.) reach all CPUs
	 * before the SGI fires and the target reads them. */
	__asm__ volatile("dsb ish" ::: "memory");
	uint32_t val = ((1u << cpu_id) << 16) | 0;
	*(volatile uint32_t *)(gicd + GICD_SGIR) = val;
	__asm__ volatile("dsb sy" ::: "memory");
}

void arch_smp_ipi_ack(void)
{
	/* EOI for SGI is handled by the GIC irq_dispatch path */
}
