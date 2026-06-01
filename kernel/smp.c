#include <kernel/smp.h>
#include <kernel/hal.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/irq.h>
#include <kernel/kprintf.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/config.h>
#include <kernel/fdt.h>
#include <kernel/board.h>
#include <asm/mmu.h>

int smp_ncpus = 1;
cpu_context_t *all_cpus[MAX_CPUS];

/* Non-static so arch code can reference them */
cpu_context_t  ap_cpu_ctxs  [MAX_CPUS];
uint8_t        ap_kstacks   [MAX_CPUS][KSTACK_SIZE] __attribute__((aligned(16)));
uint8_t        ap_irq_stacks[MAX_CPUS][KSTACK_SIZE] __attribute__((aligned(16)));
process_t      ap_idle_procs[MAX_CPUS];

/*
 * Separate initial trap-frames for secondary idle processes.
 * Placed here (not on the kstack) so arch_context_init() doesn't clobber
 * the live call-frames that secondary_cpu_main() uses on ap_kstacks.
 * After the first real interrupt fires, the irq_handler overwrites
 * idle->ctx with the actual kstack trapframe address.
 */
static arch_context_t ap_idle_init_ctxs[MAX_CPUS] __attribute__((aligned(16)));

/*
 * AP trampoline sync block — in .data.boot so VMA == physical address,
 * accessible from pre-MMU trampoline code via adr/pc-relative addressing.
 */
__attribute__((section(".data.boot")))
volatile struct ap_boot_sync {
	uint64_t ttbr0_phys;    /* offset  0 — aarch64: physical TTBR0 */
	uint64_t ttbr1_phys;    /* offset  8 — aarch64: physical TTBR1 */
	uint64_t sp_phys;       /* offset 16 — physical kstack top */
	int32_t  cpu_id;        /* offset 24 */
	volatile int32_t ready; /* offset 28 — AP signals when C init done */
	volatile int32_t go;    /* offset 32 — BSP releases AP */
	int32_t  _pad;          /* offset 36 — pad to 40 bytes total */
} ap_boot_sync[MAX_CPUS];

void smp_init(void)
{
	all_cpus[0] = arch_get_cpu_ctx();
	all_cpus[0]->online = 1;

#ifdef __aarch64__
	extern void mmu_clean_tables_poc(void);
	mmu_clean_tables_poc();
	fdt_info_t *fi = fdt_get_info();
	int ncpus = (int)fi->num_cpus;
#else
	/* x86_64: the ACPI MADT lists one enabled Local APIC per CPU. */
	extern int acpi_detect_cpus(void);
	int ncpus = acpi_detect_cpus();
	if (ncpus <= 0) ncpus = 1;
#endif
	if (ncpus <= 0) ncpus = 1;
	if (ncpus > MAX_CPUS) ncpus = MAX_CPUS;

	for (int i = 1; i < ncpus; i++) {
		ap_boot_sync[i].cpu_id    = i;
		ap_boot_sync[i].ready     = 0;
		ap_boot_sync[i].go        = 0;
		ap_boot_sync[i].sp_phys   = VIRT_TO_PHYS((uint64_t)(ap_kstacks[i] + KSTACK_SIZE));

#ifdef __aarch64__
		extern uint64_t kernel_l1[];
		extern uint64_t kernel_l1_high[];
		ap_boot_sync[i].ttbr0_phys = (uint64_t)kernel_l1;
		ap_boot_sync[i].ttbr1_phys = (uint64_t)kernel_l1_high;
#endif

		__asm__ volatile("" ::: "memory");

#ifdef __aarch64__
		/* The AP reads this entry with caches off (pre-MMU); our writes
		 * above are still in this CPU's cache. Clean to PoC or the AP gets
		 * stale zeros for ttbr0/ttbr1/sp. */
		{
			uint64_t s = (uint64_t)&ap_boot_sync[i] & ~63UL;
			uint64_t e = (uint64_t)&ap_boot_sync[i] + sizeof(ap_boot_sync[i]);
			for (uint64_t a = s; a < e; a += 64)
				__asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
			__asm__ volatile("dsb sy" ::: "memory");
		}
#endif

		arch_smp_boot_cpu(i);

		/* Wait up to ~100ms for AP to become ready */
		for (int t = 0; t < 10000000 && !ap_boot_sync[i].ready; t++)
			__asm__ volatile("" ::: "memory");

		if (!ap_boot_sync[i].ready) {
			kprintf("SMP: CPU%d timed out\n", i);
			continue;
		}

		ap_boot_sync[i].go = 1;
		__asm__ volatile("" ::: "memory");

		smp_ncpus++;
	}

	kprintf("SMP: %d/%d CPUs online\n", smp_ncpus, ncpus);
}

void smp_ipi_resched(int cpu_id)
{
	if (cpu_id == (int)arch_get_cpu_ctx()->cpu_id) return;
	if (cpu_id < MAX_CPUS && all_cpus[cpu_id])
		all_cpus[cpu_id]->need_resched = 1;
	arch_smp_ipi_send(cpu_id);
}

/* SGI #0 handler — aarch64 IPI */
void smp_ipi_handler(void)
{
	arch_get_cpu_ctx()->need_resched = 1;
	arch_smp_ipi_ack();
}

static void ap_idle_loop(void)
{
	/* See idle_loop() in sched.c: reschedule on need_resched instead of
	 * sleeping until the next tick. */
	for (;;) {
		cpu_context_t *c = arch_get_cpu_ctx();
		if (c->need_resched) { c->need_resched = 0; arch_yield(); }
		else arch_halt();
	}
}

void secondary_cpu_main(int cpu_id)
{
	cpu_context_t *cpu = &ap_cpu_ctxs[cpu_id];
	memset(cpu, 0, sizeof(*cpu));
	cpu->self         = cpu;
	cpu->cpu_id       = (uint64_t)cpu_id;
	cpu->magic        = 0xCAFEBABE;
	cpu->irq_stack    = ap_irq_stacks[cpu_id] + KSTACK_SIZE;

	process_t *p = &ap_idle_procs[cpu_id];
	memset(p, 0, sizeof(*p));
	p->pid          = 0;
	p->state        = PROC_RUNNING;
	p->on_cpu       = 1;
	p->kstack       = ap_kstacks[cpu_id];
	p->vfork_parent = -1;
	memcpy(p->name, "idle", 5);

	uint8_t *sp_top = ap_kstacks[cpu_id] + KSTACK_SIZE;
	arch_context_init(&ap_idle_init_ctxs[cpu_id], ap_idle_loop, (uint64_t)sp_top);
	p->ctx          = &ap_idle_init_ctxs[cpu_id];

	cpu->current    = p;
	cpu->kstack_top = sp_top;
	cpu->idle       = p;
	cpu->online     = 1;

	/* Must set all_cpus BEFORE arch_secondary_init (which reads it) */
	all_cpus[cpu_id] = cpu;

	arch_secondary_init(cpu_id);
	board_secondary_irq_init();

	/* Ensure all cpu_context_t writes and all_cpus[] are globally visible
	 * before BSP reads ready=1 and starts dispatching to this CPU. */
	smp_mb();

	ap_boot_sync[cpu_id].ready = 1;
	__asm__ volatile("" ::: "memory");

	while (!ap_boot_sync[cpu_id].go)
		__asm__ volatile("" ::: "memory");

	irq_enable();

	/* idle loop — reschedule on need_resched, else sleep until an interrupt */
	while (1) {
		cpu_context_t *c = arch_get_cpu_ctx();
		if (c->need_resched) { c->need_resched = 0; arch_yield(); }
		else arch_halt();
	}
}
