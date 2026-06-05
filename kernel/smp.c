#include <kernel/smp.h>
#include <kernel/hal.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/irq.h>
#include <kernel/sync.h>
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

/*
 * Per-CPU bring-up state. all_cpus[] and ap_boot_sync[] stay statically sized by
 * MAX_CPUS; the bulky per-CPU memory below is allocated in smp_init() sized to
 * the detected CPU count, so a 4-core box doesn't carry MAX_CPUS worth of kernel
 * stacks. Non-static so arch code can reference them.
 */
cpu_context_t  *ap_cpu_ctxs;       /* contiguous (small per element) */
uint8_t       **ap_kstacks;        /* per-CPU: each a physically-contiguous stack */
uint8_t       **ap_irq_stacks;     /* (kernel stacks must be phys-contiguous for the */
process_t      **ap_idle_procs;    /*  pre-MMU AP entry — so no single big array cap) */

/*
 * Separate initial trap-frames for secondary idle processes — kept off the
 * kstack so arch_context_init() doesn't clobber the live call-frames that
 * secondary_cpu_main() uses on ap_kstacks.
 */
static arch_context_t *ap_idle_init_ctxs;

/* Allocate `n` zeroed, page-rounded bytes from the buddy; returns a direct-map
 * virtual pointer (VIRT_TO_PHYS works on it for the trampoline's sp_phys). */
static void *percpu_alloc(uint64_t n)
{
	uint64_t pages = (n + PAGE_SIZE - 1) / PAGE_SIZE;
	int order = 0;
	while (((uint64_t)1 << order) < pages) order++;
	void *p = buddy_alloc(order);
	if (!p) return NULL;
	void *v = (void *)PHYS_TO_VIRT((uint64_t)p);
	memset(v, 0, pages * PAGE_SIZE);
	return v;
}

/*
 * AP trampoline sync block — in .bss.boot (NOLOAD, zeroed by boot.S) so VMA ==
 * physical address and it's reachable from pre-MMU trampoline code, without
 * bloating the binary by MAX_CPUS*sizeof zeros the way .data.boot would.
 */
__attribute__((section(".bss.boot")))
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

	/* Size the bulky per-CPU arrays to the detected count. If allocation fails
	 * we fall back to uniprocessor rather than risk a half-built AP. */
	if (ncpus > 1) {
		ap_cpu_ctxs       = percpu_alloc((uint64_t)ncpus * sizeof(cpu_context_t));
		ap_idle_init_ctxs = percpu_alloc((uint64_t)ncpus * sizeof(arch_context_t));
		ap_kstacks        = percpu_alloc((uint64_t)ncpus * sizeof(uint8_t *));
		ap_irq_stacks     = percpu_alloc((uint64_t)ncpus * sizeof(uint8_t *));
		ap_idle_procs     = percpu_alloc((uint64_t)ncpus * sizeof(process_t *));
		int ok = ap_cpu_ctxs && ap_idle_init_ctxs && ap_kstacks &&
		         ap_irq_stacks && ap_idle_procs;
		/* Per-CPU stacks/procs allocated individually so no single allocation
		 * caps the core count (one big array would hit the buddy's 32 MB limit
		 * near ~2048 CPUs). Each kstack is its own contiguous block. */
		for (int i = 1; ok && i < ncpus; i++) {
			ap_kstacks[i]    = percpu_alloc(KSTACK_SIZE);
			ap_irq_stacks[i] = percpu_alloc(KSTACK_SIZE);
			ap_idle_procs[i] = percpu_alloc(sizeof(process_t));
			if (!ap_kstacks[i] || !ap_irq_stacks[i] || !ap_idle_procs[i]) ok = 0;
		}
		if (!ok) {
			kprintf("SMP: per-CPU alloc failed, staying uniprocessor\n");
			ncpus = 1;
		}
	}

	for (int i = 1; i < ncpus; i++) {
		ap_boot_sync[i].cpu_id    = i;
		ap_boot_sync[i].ready     = 0;
		ap_boot_sync[i].go        = 0;
		ap_boot_sync[i].sp_phys   = VIRT_TO_PHYS((uint64_t)(ap_kstacks[i] + KSTACK_SIZE));

#ifdef __aarch64__
		extern uint64_t kernel_l0[];
		extern uint64_t kernel_l0_high[];
		ap_boot_sync[i].ttbr0_phys = (uint64_t)kernel_l0;   /* 4-level: L0 roots */
		ap_boot_sync[i].ttbr1_phys = (uint64_t)kernel_l0_high;
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

/* membarrier(2): the reschedule IPI is reused as the barrier vehicle. The
 * broadcaster raises membarrier_req, sends an IPI to every other online CPU,
 * and waits for each handler to run a full barrier and bump membarrier_acks.
 * One in-flight broadcast at a time (membarrier_lock). */
static spinlock_t membarrier_lock;
static volatile uint32_t membarrier_req;
static volatile uint32_t membarrier_acks;

void smp_membarrier(void)
{
	if (smp_ncpus <= 1) { smp_mb(); return; }

	uint64_t f = spin_lock_irq(&membarrier_lock);
	int self = (int)arch_get_cpu_ctx()->cpu_id;
	uint32_t expect = 0;
	membarrier_acks = 0;
	smp_mb();
	membarrier_req = 1;
	smp_mb();
	for (int i = 0; i < MAX_CPUS; i++) {
		if (i == self || !all_cpus[i] || !all_cpus[i]->online) continue;
		expect++;
		arch_smp_ipi_send(i);
	}
	/* Bounded wait so a wedged CPU never hangs the caller; the local barrier
	 * below still orders this CPU's accesses regardless. */
	for (uint64_t t = 0; membarrier_acks < expect && t < 100000000UL; t++)
		__asm__ volatile("" ::: "memory");
	membarrier_req = 0;
	smp_mb();
	spin_unlock_irq(&membarrier_lock, f);
	smp_mb();
}

/* If a membarrier broadcast is in flight, run a full barrier and ack it. Called
 * from every IPI entry — the SGI handler below (aarch64) and the x86 IPI stub. */
void smp_ipi_membarrier_ack(void)
{
	if (membarrier_req) {
		smp_mb();
		atomic_fetch_add32(&membarrier_acks, 1);
	}
}

/* SGI #0 handler — aarch64 IPI. Shared between resched and membarrier. */
void smp_ipi_handler(void)
{
	smp_ipi_membarrier_ack();
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

	process_t *p = ap_idle_procs[cpu_id];
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
