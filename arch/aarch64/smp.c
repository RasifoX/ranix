#include <kernel/hal.h>
#include <kernel/smp.h>
#include <kernel/kprintf.h>
#include <kernel/board.h>
#include <kernel/fdt.h>
#include <asm/mmu.h>

#define PSCI_CPU_ON  0xC4000003UL
#define GICD_SGIR    0xF00

void arch_smp_boot_cpu(int cpu_id)
{
	extern void secondary_trampoline(void);
	uint64_t entry_phys = (uint64_t)secondary_trampoline;

	/* Prefer the spin-table release address the firmware actually published
	 * in the DTB (dynamic, per-board); fall back to the board's static hint. */
	uint64_t rel = 0;
	if (cpu_id >= 0 && cpu_id < 8) {
		fdt_info_t *fi = fdt_get_info();
		if (fi && fi->cpu_release[cpu_id])
			rel = fi->cpu_release[cpu_id];
		else
			rel = board_get_config()->smp_release_addr[cpu_id];
	}

	if (rel) {
		/* Spin-table (some Pi4 firmware): the secondary spins with caches/MMU
		 * off, polling its release address. Publish the entry point to the
		 * point of coherency, then wake it with SEV. */
		volatile uint64_t *slot = (volatile uint64_t *)PHYS_TO_VIRT(rel);
		*slot = entry_phys;
		__asm__ volatile("dc cvac, %0\n dsb sy\n sev\n" :: "r"(slot) : "memory");
		return;
	}

	/* PSCI CPU_ON. Conduit comes from the DTB /psci method: real Pi4 firmware
	 * provides PSCI at EL3 via SMC; QEMU virt uses HVC. (Ranix took over EL2,
	 * so hvc on Pi4 would trap into our own empty EL2 vector — must use smc.) */
	fdt_info_t *fi = fdt_get_info();
	int use_smc = fi && fi->psci_use_smc;
	uint64_t mpidr      = (uint64_t)cpu_id;
	uint64_t context_id = (uint64_t)cpu_id;
	register uint64_t r0 __asm__("x0") = PSCI_CPU_ON;
	register uint64_t r1 __asm__("x1") = mpidr;
	register uint64_t r2 __asm__("x2") = entry_phys;
	register uint64_t r3 __asm__("x3") = context_id;
	if (use_smc)
		__asm__ volatile("smc #0" :: "r"(r0), "r"(r1), "r"(r2), "r"(r3) : "memory");
	else
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
