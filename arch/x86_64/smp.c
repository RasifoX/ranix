#include <kernel/hal.h>
#include <kernel/smp.h>
#include <kernel/kprintf.h>
#include <kernel/mm/vm.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/config.h>
#include <asm/gdt.h>
#include <asm/idt.h>
#include <asm/mmu.h>
#include <asm/io.h>
#include <kernel/tick.h>
#include <asm/cpu.h>
#include <kernel/sync.h>
void pic8259_mask(uint32_t irq);
#include <kernel/board.h>
#include <kernel/sync.h>
#include <kernel/irq.h>
#include <kernel/cpumask.h>

/* LAPIC register offsets */
#define LAPIC_ID         0x020
#define LAPIC_EOI        0x0B0
#define LAPIC_SPIV       0x0F0
#define LAPIC_LVT_TIMER  0x320
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0
#define LAPIC_ICR_LO     0x300
#define LAPIC_ICR_HI     0x310

#define LVT_TIMER_PERIODIC 0x20000
#define LVT_MASKED         0x10000
#define LAPIC_LVT_LINT0    0x350
#define LAPIC_LVT_LINT1    0x360
#define LVT_DM_EXTINT      0x700
#define LVT_DM_NMI         0x400
#define VEC_LAPIC_TIMER    0xF1

/* ICR delivery mode bits */
#define ICR_INIT         0x00004500
#define ICR_SIPI         0x00004600
#define ICR_ASSERT       0x00004000
#define ICR_LEVEL        0x00008000
#define ICR_FIXED        0x00000000

#define LAPIC_PHYS       0xFEE00000UL
#define TRAMP_PHYS       0x7000UL
#define DATA_OFF         0xF00UL

#define TDATA_PML4   0x00
#define TDATA_SP     0x08
#define TDATA_CPU_ID 0x10

static volatile uint32_t *lapic;

/* logical cpu index -> physical APIC id (filled by ACPI MADT parse) */

static inline uint32_t lapic_read(uint32_t reg)        { return lapic[reg / 4]; }
static inline void     lapic_write(uint32_t reg, uint32_t val) { lapic[reg / 4] = val; }

static void lapic_init_bsp(void)
{
	lapic = (volatile uint32_t *)ioremap(LAPIC_PHYS, 0x1000,
	                                     VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
	lapic_write(LAPIC_SPIV, lapic_read(LAPIC_SPIV) | 0x100 | 0xFF);
	/* Firmware is free to leave these masked. The 8259 reaches the BSP only
	 * through LINT0 as ExtINT, so without this no legacy IRQ arrives. */
	lapic_write(LAPIC_LVT_LINT0, LVT_DM_EXTINT);
	lapic_write(LAPIC_LVT_LINT1, LVT_DM_NMI);
}

static void io_delay(void)
{
	for (volatile int i = 0; i < 100000; i++)
		__asm__ volatile("nop");
}

/* Periodic LAPIC-timer init count for one TIMER_HZ tick, measured against the
 * PIT once on the BSP. Each CPU has its own local timer but they run at the
 * same frequency, so one calibration serves all. */
static uint32_t lapic_timer_initcnt;

static void lapic_timer_calibrate(void)
{
	if (!lapic || lapic_timer_initcnt) return;

	uint64_t flags = irq_save();
	lapic_write(LAPIC_TIMER_DIV, 0x3);                 /* divide by 16 */

	/* PIT channel 2, mode 0 (one-shot), count = 1/TIMER_HZ second. */
	uint16_t pit_cnt = (uint16_t)(1193182UL / TIMER_HZ);
	outb(0x61, (inb(0x61) & 0xFC) | 0x01);             /* gate on, speaker off */
	outb(0x43, 0xB0);                                  /* ch2, lo/hi, mode 0 */
	outb(0x42, pit_cnt & 0xFF);
	outb(0x42, pit_cnt >> 8);

	lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);
	uint64_t guard = 0;
	while (!(inb(0x61) & 0x20))                         /* wait PIT ch2 OUT high */
		if (++guard > 100000000UL) break;
	uint32_t remaining = lapic_read(LAPIC_TIMER_CUR);
	lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);

	lapic_timer_initcnt = 0xFFFFFFFF - remaining;
	/* From Sky Lake on, the local timer runs from the core crystal, which CPUID 15h
	 * names (or which is 24 MHz on the clients that leave it out). One PIT period
	 * measured it 0.4 % off on Casper: every sleep and the uptime ran slow. */
	{
		uint32_t a, b, c, d, max;
		__asm__ volatile("cpuid" : "=a"(max), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
		uint64_t crystal = 0;
		if (max >= 0x15) {
			__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x15), "c"(0));
			crystal = c;
			if (!crystal && a && b) {
				uint32_t a1, b1, c1, d1;
				__asm__ volatile("cpuid" : "=a"(a1), "=b"(b1), "=c"(c1), "=d"(d1) : "a"(1));
				uint32_t fam = (a1 >> 8) & 15, model = ((a1 >> 4) & 15) | ((a1 >> 12) & 0xF0);
				if (fam == 6 && (model == 0x4E || model == 0x5E || model == 0x8E || model == 0x9E ||
				                 model == 0xA5 || model == 0xA6))
					crystal = 24000000;
			}
		}
		uint32_t exact = crystal ? (uint32_t)(crystal / 16 / TIMER_HZ) : 0;
		uint32_t m = lapic_timer_initcnt;
		if (exact && exact > m - m / 50 && exact < m + m / 50) {
			kprintf(KERN_INFO "timer: lapic from the %lu Hz crystal (measured %u)\n",
			        (unsigned long)crystal, m);
			lapic_timer_initcnt = exact;
		}
	}
	irq_restore(flags);
	kprintf(KERN_INFO "timer: lapic %u ticks per %d Hz\n", lapic_timer_initcnt, TIMER_HZ);
}

/* Start this CPU's periodic local timer. Calibrates lazily on first use. */
void lapic_timer_start(void)
{
	if (!lapic) return;
	if (!lapic_timer_initcnt) lapic_timer_calibrate();
	if (!lapic_timer_initcnt) return;
	lapic_write(LAPIC_TIMER_DIV, 0x3);
	lapic_write(LAPIC_LVT_TIMER, VEC_LAPIC_TIMER | LVT_TIMER_PERIODIC);
	lapic_write(LAPIC_TIMER_INIT, lapic_timer_initcnt);
}

void arch_tick_slow(int slow)
{
	if (lapic && lapic_timer_initcnt)
		lapic_write(LAPIC_TIMER_INIT, lapic_timer_initcnt * (slow ? TICK_SLOW : 1));
}

/* Move the BSP's global tick from the PIT to its LAPIC timer, so the tick
 * survives chipsets that gate the 8254 and shares one path with the APs. On any
 * failure the PIT stays in charge. */
void lapic_bsp_tick_takeover(void)
{
	if (!lapic) lapic_init_bsp();
	lapic_timer_calibrate();
	if (!lapic_timer_initcnt || lapic_timer_initcnt == 0xFFFFFFFF) {
		lapic_timer_initcnt = 0;
		kprintf(KERN_WARN "timer: lapic calibration failed, keeping pit tick\n");
		return;
	}
	pic8259_mask(0);
	lapic_timer_start();
}

/* Vector 0xF1 handler — scheduler tick. The BSP's carries the global tick. */
void lapic_timer_irq(void)
{
	if (arch_get_cpu_ctx()->cpu_id == 0) tick_notify();
	else tick_notify_local();
	lapic_write(LAPIC_EOI, 0);
}


static void setup_trampoline(int cpu_id)
{
	uint64_t tramp_virt = PHYS_TO_VIRT(TRAMP_PHYS);
	size_t   len = (size_t)(trampoline_end - trampoline_start);
	memcpy((void *)tramp_virt, trampoline_start, len);

	uint8_t *data = (uint8_t *)(tramp_virt + DATA_OFF);

	/* Top-level table the AP loads into CR3 — PML5 under 5-level, else PML4.
	 * Same VIRT_TO_PHYS convention as the BSP path that already boots APs. */
	uint64_t pml4_phys = la57_enabled ? VIRT_TO_PHYS((uint64_t)pml5)
	                                   : VIRT_TO_PHYS((uint64_t)pml4);

	/* Virtual stack top for C code (secondary_cpu_main uses virtual addresses) */
	uint64_t sp_virt = (uint64_t)(ap_kstacks[cpu_id] + KSTACK_SIZE);

	*(uint64_t *)(data + TDATA_PML4)   = pml4_phys;
	*(uint64_t *)(data + TDATA_SP)     = sp_virt;
	*(uint32_t *)(data + TDATA_CPU_ID) = (uint32_t)cpu_id;
}

void arch_smp_boot_cpu(int cpu_id)
{
	if (!lapic) lapic_init_bsp();
	lapic_timer_calibrate();   /* once, on the BSP, before any AP starts */

	setup_trampoline(cpu_id);

	uint32_t apic_id = cpu_apic_id[cpu_id];

	/* INIT-SIPI-SIPI sequence per Intel spec */
	lapic_write(LAPIC_ICR_HI, apic_id << 24);
	lapic_write(LAPIC_ICR_LO, ICR_INIT | ICR_ASSERT | ICR_LEVEL);
	io_delay();
	lapic_write(LAPIC_ICR_HI, apic_id << 24);
	lapic_write(LAPIC_ICR_LO, ICR_INIT | ICR_LEVEL);
	io_delay();

	lapic_write(LAPIC_ICR_HI, apic_id << 24);
	lapic_write(LAPIC_ICR_LO, ICR_SIPI | (TRAMP_PHYS >> 12));
	io_delay();
	lapic_write(LAPIC_ICR_HI, apic_id << 24);
	lapic_write(LAPIC_ICR_LO, ICR_SIPI | (TRAMP_PHYS >> 12));
}

void arch_secondary_init(int cpu_id)
{
	cpu_context_t *cpu = all_cpus[cpu_id];

	/* Set both GS_BASE and KERNEL_GS_BASE */
	uint64_t gs_base = (uint64_t)cpu;
	__asm__ volatile(
		"movl $0xC0000101, %%ecx\n"
		"movq %0, %%rax\n"
		"movq %%rax, %%rdx\n"
		"shrq $32, %%rdx\n"
		"wrmsr\n"
		"movl $0xC0000102, %%ecx\n"
		"wrmsr\n"
		:: "r"(gs_base) : "rax", "ecx", "rdx"
	);

	/* Load per-CPU GDT and shared IDT */
	gdt_init(cpu_id);
	idt_load();

	/* Enable SSE; CR0.WP so ring-0 stores honour the read-only PTE bit (see
	 * arch_cpu_init — kernel writes to COW user pages must fault, not write through). */
	uint64_t cr0, cr4_val;
	__asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
	cr0 &= ~(1ULL << 2);
	cr0 |= (1ULL << 1) | (1ULL << 16);
	__asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
	__asm__ volatile("mov %%cr4, %0" : "=r"(cr4_val));
	cr4_val |= (3ULL << 9);
	/* The entry stubs touch ymm on every CPU once the boot CPU has switched AVX on */
	arch_pat_init();
	arch_hwp_init();
	if (avx_on) cr4_val |= (1ULL << 18);
	__asm__ volatile("mov %0, %%cr4" :: "r"(cr4_val));
	if (avx_on)
		__asm__ volatile("xsetbv" :: "a"(7), "d"(0), "c"(0));

	/* Enable AP LAPIC so it can receive IPIs */
	if (lapic)
		lapic_write(LAPIC_SPIV, lapic_read(LAPIC_SPIV) | 0x100 | 0xFF);

	/* Enable syscall instruction on this AP (EFER.SCE + LSTAR/STAR/SFMASK) */
	arch_syscall_init();

	/* Per-CPU periodic timer — the PIT only reaches the BSP, so without this a
	 * task on a secondary is never preempted and never returns to the kernel to
	 * take a pending signal. */
	lapic_timer_start();
}

void lapic_eoi(void)
{
	lapic_write(LAPIC_EOI, 0);
}

uint32_t lapic_self_id(void)
{
	return lapic_read(LAPIC_ID) >> 24;
}

void arch_smp_ipi_send(int cpu_id)
{
	if (!lapic) return;
	lapic_write(LAPIC_ICR_HI, cpu_apic_id[cpu_id] << 24);
	lapic_write(LAPIC_ICR_LO, ICR_FIXED | 0xF0);
}

void arch_smp_ipi_ack(void)
{
	if (lapic) lapic_write(LAPIC_EOI, 0);
}

/*
 * Cross-CPU TLB shootdown (vector 0xF2). x86 invlpg/CR3 are CPU-local, so a
 * PTE downgrade (unmap, COW write-protect, mprotect) leaves sibling CPUs
 * running in the same address space with stale TLB entries until they happen
 * to reload CR3. A sibling thread on another CPU never does (arch_vm_switch
 * skips the reload for an unchanged root), so it can keep writing through a
 * stale writable entry. The shootdown forces every other online CPU to flush.
 *
 * Deadlock-safety mirrors the aarch64 IRQ-on model: the body runs IRQ-on, so a
 * sibling sitting in its own syscall takes the IPI and acks promptly; the only
 * IRQ-off windows are short spin_lock_irq sections, so an ack is always bounded.
 * The single shootdown lock keeps one broadcast in flight, so the waiter never
 * has to ack a peer while waiting. Callers MUST NOT hold vs->fault_lock here: a
 * sibling spinning IRQ-off on that lock could not ack — issue the shootdown only
 * after the lock is dropped.
 */
#define VEC_TLB_SHOOTDOWN 0xF2

static spinlock_t tlb_shoot_lock;
static cpumask_t tlb_ack_mask;

/* Atomically clear one bit in a cpumask word (the targets ack concurrently). */
static void cpumask_atomic_clear(int cpu, cpumask_t *m)
{
	volatile uint64_t *w = &m->bits[cpu / BITS_PER_LONG];
	uint64_t bit = 1ULL << (cpu % BITS_PER_LONG);
	uint64_t old;
	do { old = *w; } while (atomic_cmpxchg64(w, old, old & ~bit) != old);
}

/* Vector 0xF2 handler: flush the whole non-global TLB (CR3 reload) and ack. */
void tlb_shootdown_handler(void)
{
	uint64_t cr3;
	__asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
	int self = (int)arch_get_cpu_ctx()->cpu_id;
	cpumask_atomic_clear(self, &tlb_ack_mask);
	if (lapic) lapic_write(LAPIC_EOI, 0);
}

static void tlb_ipi_send(int cpu_id)
{
	/* Wait for any prior IPI to leave the ICR (delivery-status, bit 12) so we
	 * don't overwrite an in-flight command. */
	while (lapic_read(LAPIC_ICR_LO) & (1u << 12))
		__asm__ volatile("pause" ::: "memory");
	lapic_write(LAPIC_ICR_HI, cpu_apic_id[cpu_id] << 24);
	lapic_write(LAPIC_ICR_LO, ICR_FIXED | VEC_TLB_SHOOTDOWN);
}

void arch_tlb_shootdown_all(void)
{
	if (!lapic || smp_ncpus <= 1) return;

	/* Hold the shootdown lock with IRQs ENABLED: while another CPU holds it and
	 * waits for our ack, we must keep taking its 0xF2 IPI. spin_lock spins with
	 * the caller's IRQ state, so enabling them first means a contending shootdown
	 * cannot deadlock us (each would otherwise spin IRQ-off, neither able to ack
	 * the other). The 0xF2 handler is lock-free, so taking it mid-spin is safe. */
	uint64_t f = irq_save();
	/* Turning IRQs on under a caller's IRQ-off spinlock lets the timer tick
	 * take that same lock on this CPU. Callers must drop such locks first. */
	{
		if (!(f & 0x200) && spin_held_count() > 0) {
			spin_note_violation(&tlb_shoot_lock, __builtin_return_address(0));
			static int told;
			if (told++ < 3) {          /* who holds what: the note alone names only us */
				kprintf(KERN_ERR "tlb: shootdown with IRQs off under a lock, from %p %p\n",
				        __builtin_return_address(0), __builtin_return_address(1));
				spin_dump_held();
			}
		}
	}
	irq_enable();
	spin_lock(&tlb_shoot_lock);

	int self = (int)arch_get_cpu_ctx()->cpu_id;
	cpumask_t mask;
	cpumask_clear(&mask);
	for (int i = 0; i < smp_ncpus; i++) {
		if (i == self || !all_cpus[i] || !all_cpus[i]->online) continue;
		cpumask_set_cpu(i, &mask);
	}
	if (cpumask_empty(&mask)) { spin_unlock(&tlb_shoot_lock); irq_restore(f); return; }

	tlb_ack_mask = mask;
	smp_mb();
	/* The ICR is a single per-LAPIC register written as a HI+LO pair. Send with
	 * IRQs masked so a nested IPI send (e.g. a timer-driven resched IPI) can't
	 * clobber the dest/vector between our two writes and misdeliver — or drop —
	 * the shootdown. The send is a handful of writes; a target waiting for our
	 * ack just spins those few cycles. */
	uint64_t sf = irq_save();
	int cpu;
	for_each_cpu(cpu, &mask)
		tlb_ipi_send(cpu);
	irq_restore(sf);

	/* We hold the only shootdown lock, so no peer broadcast can be in flight and
	 * we owe no ack while waiting. A target re-enables IRQs within a bounded
	 * spin_lock_irq window (or is already IRQ-on in its syscall body / idle
	 * halt), so this always completes; the large guard only avoids a hard wedge
	 * during bring-up. */
	uint64_t t = 0;
	while (!cpumask_empty(&tlb_ack_mask)) {
		__asm__ volatile("pause" ::: "memory");
		if (++t > 5000000000UL) {
			kprintf(KERN_ERR "smp: tlb shootdown stuck, %d acks pending\n",
			        cpumask_weight(&tlb_ack_mask));
			break;
		}
	}
	spin_unlock(&tlb_shoot_lock);
	irq_restore(f);
}
