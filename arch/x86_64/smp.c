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
#include <kernel/board.h>
#include <kernel/sync.h>
#include <kernel/irq.h>

/* LAPIC register offsets */
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
extern uint32_t cpu_apic_id[];

static inline uint32_t lapic_read(uint32_t reg)        { return lapic[reg / 4]; }
static inline void     lapic_write(uint32_t reg, uint32_t val) { lapic[reg / 4] = val; }

static void lapic_init_bsp(void)
{
	lapic = (volatile uint32_t *)ioremap(LAPIC_PHYS, 0x1000,
	                                     VM_READ | VM_WRITE | VM_NOCACHE | VM_DEVICE);
	lapic_write(LAPIC_SPIV, lapic_read(LAPIC_SPIV) | 0x100 | 0xFF);
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
	irq_restore(flags);
	kprintf("LAPIC timer: %u ticks per %d Hz\n", lapic_timer_initcnt, TIMER_HZ);
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

/* Vector 0xF1 handler — per-CPU scheduler tick. */
void lapic_timer_irq(void)
{
	tick_notify_local();
	lapic_write(LAPIC_EOI, 0);
}

extern char trampoline_start[];
extern char trampoline_end[];

static void setup_trampoline(int cpu_id)
{
	uint64_t tramp_virt = PHYS_TO_VIRT(TRAMP_PHYS);
	size_t   len = (size_t)(trampoline_end - trampoline_start);
	memcpy((void *)tramp_virt, trampoline_start, len);

	uint8_t *data = (uint8_t *)(tramp_virt + DATA_OFF);

	/* PML4 physical address */
	extern uint64_t pml4[];
	uint64_t pml4_phys = VIRT_TO_PHYS((uint64_t)pml4);

	/* Virtual stack top for C code (secondary_cpu_main uses virtual addresses) */
	extern uint8_t **ap_kstacks;
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
	extern void idt_load(void);
	gdt_init(cpu_id);
	idt_load();

	/* Enable SSE */
	uint64_t cr0, cr4_val;
	__asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
	cr0 &= ~(1ULL << 2);
	cr0 |= (1ULL << 1);
	__asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
	__asm__ volatile("mov %%cr4, %0" : "=r"(cr4_val));
	cr4_val |= (3ULL << 9);
	__asm__ volatile("mov %0, %%cr4" :: "r"(cr4_val));

	/* Enable AP LAPIC so it can receive IPIs */
	if (lapic)
		lapic_write(LAPIC_SPIV, lapic_read(LAPIC_SPIV) | 0x100 | 0xFF);

	/* Enable syscall instruction on this AP (EFER.SCE + LSTAR/STAR/SFMASK) */
	extern void arch_syscall_init(void);
	arch_syscall_init();

	/* Per-CPU periodic timer — the PIT only reaches the BSP, so without this a
	 * task on a secondary is never preempted and never returns to the kernel to
	 * take a pending signal. */
	lapic_timer_start();
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
