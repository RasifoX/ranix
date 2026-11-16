/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The local APIC of a KVM vcpu, in the kernel (the "split" interrupt chip:
 * the IOAPIC and the PIC stay with the VMM). QEMU's pc and q35 machines need
 * it with KVM: they refuse a local APIC in userspace.
 *
 * The register page is kept as KVM_GET_LAPIC hands it out, each register at
 * its xAPIC offset. The guest reaches it through its MMIO page (an EPT
 * violation at APIC_BASE, decoded in kvm_emul.c); CR8 is TPR[7:4].
 *
 * Other threads deliver into it (an IPI from another vcpu, an MSI from the
 * VMM or an irqfd): they set IRR and TMR bits with atomic operations and kick
 * the vcpu (kvm_vcpu_kick), under vm->irq_lock, which also keeps vcpus[]
 * steady. Only the vcpu's own thread moves bits from IRR to ISR, ends them
 * (EOI) and runs the timer.
 *
 * The timer counts an APIC bus of 1 GHz through the divide register, in
 * one-shot and periodic mode; the guest's TSC deadline mode is not offered in
 * CPUID. A running guest is stopped at the deadline by the VMX preemption timer
 * (vmx.c), a halted one sleeps until it.
 *
 * Provenance: register layout and behaviour from the Intel SDM Vol. 3 chapter
 * 11 (APIC) and 10.11 (MSI format); the KVM_GET/SET_LAPIC layout and the split
 * irqchip division of work from Linux's public KVM API description (api.rst).
 * Original code. */

#include <asm/kvm.h>
#include <uapi/kvm.h>
#include <kernel/proc.h>
#include <kernel/sched.h>
#include <kernel/smp.h>
#include <kernel/time.h>
#include <kernel/errno.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>

#define APIC_ID     0x20
#define APIC_LVR    0x30
#define APIC_TPR    0x80
#define APIC_APR    0x90
#define APIC_PPR    0xA0
#define APIC_EOI    0xB0
#define APIC_LDR    0xD0
#define APIC_DFR    0xE0
#define APIC_SVR    0xF0
#define APIC_ISR    0x100
#define APIC_TMR    0x180
#define APIC_IRR    0x200
#define APIC_ESR    0x280
#define APIC_ICR    0x300
#define APIC_ICR2   0x310
#define APIC_LVTT   0x320
#define APIC_LVTTHM 0x330
#define APIC_LVTPC  0x340
#define APIC_LVT0   0x350
#define APIC_LVT1   0x360
#define APIC_LVTERR 0x370
#define APIC_TMICT  0x380
#define APIC_TMCCT  0x390
#define APIC_TDCR   0x3E0

#define SVR_ENABLE   (1U << 8)
#define LVT_MASKED   (1U << 16)
#define LVT_PERIODIC (1U << 17)
#define DM_FIXED  0
#define DM_LOWEST 1
#define DM_NMI    4
#define DM_INIT   5
#define DM_SIPI   6
#define DM_EXTINT 7

static uint32_t *R(kvm_lapic_t *a, uint32_t off) { return &a->reg[off >> 2]; }

/* a bit of the 256-bit ISR/TMR/IRR vector at base */
static uint32_t *vword(kvm_lapic_t *a, uint32_t base, int vec) { return R(a, base + (uint32_t)(vec >> 5) * 16); }

static void vbit_set(kvm_lapic_t *a, uint32_t base, int vec)
{
	__atomic_fetch_or(vword(a, base, vec), 1U << (vec & 31), __ATOMIC_SEQ_CST);
}

static void vbit_clear(kvm_lapic_t *a, uint32_t base, int vec)
{
	__atomic_fetch_and(vword(a, base, vec), ~(1U << (vec & 31)), __ATOMIC_SEQ_CST);
}

static int vbit(kvm_lapic_t *a, uint32_t base, int vec) { return (*vword(a, base, vec) >> (vec & 31)) & 1; }

/* the highest vector set at base, or -1 */
static int highest(kvm_lapic_t *a, uint32_t base)
{
	for (int i = 7; i >= 0; i--) {
		uint32_t w = __atomic_load_n(R(a, base + (uint32_t)i * 16), __ATOMIC_ACQUIRE);
		if (w) return i * 32 + 31 - __builtin_clz(w);
	}
	return -1;
}

static uint32_t ppr(kvm_lapic_t *a)
{
	int isrv = highest(a, APIC_ISR);
	uint32_t tpr = *R(a, APIC_TPR) & 0xFF;
	uint32_t isrc = isrv < 0 ? 0 : (uint32_t)isrv & 0xF0;
	return (tpr & 0xF0) >= isrc ? tpr : isrc;
}

void kvm_lapic_reset(kvm_vcpu_t *v)
{
	kvm_lapic_t *a = v->apic;
	memset(a->reg, 0, sizeof(a->reg));
	*R(a, APIC_ID) = (uint32_t)v->id << 24;
	*R(a, APIC_LVR) = 0x14 | (5U << 16);   /* integrated APIC, six LVT entries */
	*R(a, APIC_DFR) = 0xFFFFFFFF;
	*R(a, APIC_SVR) = 0xFF;
	for (uint32_t off = APIC_LVTT; off <= APIC_LVTERR; off += 0x10) *R(a, off) = LVT_MASKED;
	a->timer_end = 0;
}

int kvm_lapic_create(kvm_vcpu_t *v)
{
	v->apic = kmalloc(sizeof(kvm_lapic_t));
	if (!v->apic) return -ENOMEM;
	kvm_lapic_reset(v);
	return 0;
}

void kvm_lapic_free(kvm_vcpu_t *v)
{
	kfree(v->apic);
	v->apic = NULL;
}

/* Make v look at its interrupts: out of the guest if it runs, awake if it
 * sleeps in HLT or waits for a SIPI. kick before blocked, as the sleeper sets
 * blocked before testing kick (task_sleep_flag_until_ns). */
void kvm_vcpu_kick(kvm_vcpu_t *v)
{
	__atomic_store_n(&v->kick, 1, __ATOMIC_SEQ_CST);
	int cpu = v->guest_cpu;
	if (cpu >= 0) smp_ipi_resched(cpu);
	if (__atomic_load_n(&v->blocked, __ATOMIC_SEQ_CST) && v->thread) task_wake(v->thread);
}

/* A fixed or lowest-priority interrupt arrives at v's APIC (any thread) */
static void accept(kvm_vcpu_t *v, int vec, int level)
{
	kvm_lapic_t *a = v->apic;
	if (vec < 16) { __atomic_fetch_or(R(a, APIC_ESR), 1U << 6, __ATOMIC_SEQ_CST); return; }   /* illegal vector */
	if (level) vbit_set(a, APIC_TMR, vec);
	else vbit_clear(a, APIC_TMR, vec);
	vbit_set(a, APIC_IRR, vec);
	kvm_vcpu_kick(v);
}

/* Does v's APIC answer to dest in this destination mode? */
static int matches(kvm_vcpu_t *v, uint32_t dest, int logical)
{
	kvm_lapic_t *a = v->apic;
	if (!logical) return dest == 0xFF || dest == (*R(a, APIC_ID) >> 24);
	uint32_t ldr = *R(a, APIC_LDR) >> 24;
	if (dest == 0xFF) return 1;
	if ((*R(a, APIC_DFR) >> 28) == 0xF) return (ldr & dest) != 0;            /* flat */
	return (ldr >> 4) == (dest >> 4) && (ldr & dest & 0xF) != 0;             /* cluster */
}

/* Deliver to every matching APIC of the VM: an IPI (src set) or an MSI. */
void kvm_lapic_deliver(kvm_vm_t *vm, kvm_vcpu_t *src, uint32_t dest, int logical, int shorthand,
                       int mode, int vec, int level)
{
	uint64_t f = spin_lock_irq(&vm->irq_lock);
	for (int i = 0; i < KVM_MAX_VCPUS; i++) {
		kvm_vcpu_t *v = vm->vcpus[i];
		if (!v || !v->apic) continue;
		int hit = shorthand == 1 ? v == src :
		          shorthand == 2 ? 1 :
		          shorthand == 3 ? v != src : matches(v, dest, logical);
		if (!hit) continue;
		switch (mode) {
		case DM_FIXED:
		case DM_LOWEST:
			accept(v, vec, level);
			break;
		case DM_NMI:
			v->nmi_pending = 1;
			kvm_vcpu_kick(v);
			break;
		case DM_INIT:
			/* a SIPI before this INIT is void; one after it must survive
			 * however late the target gets to the INIT */
			__atomic_store_n(&v->sipi_pending, 0, __ATOMIC_SEQ_CST);
			__atomic_store_n(&v->init_pending, 1, __ATOMIC_SEQ_CST);
			kvm_vcpu_kick(v);
			break;
		case DM_SIPI:
			v->sipi_vector = vec;
			__atomic_store_n(&v->sipi_pending, 1, __ATOMIC_SEQ_CST);
			kvm_vcpu_kick(v);
			break;
		default:
			break;   /* SMI, ExtINT: not modelled */
		}
		if (mode == DM_LOWEST && !shorthand) break;   /* one of them takes it */
	}
	spin_unlock_irq(&vm->irq_lock, f);
}

/* An MSI write (SDM 10.11): address bits 19:12 the destination, bit 2 logical;
 * data bits 7:0 vector, 10:8 delivery mode, 15 level-triggered. */
void kvm_msi_deliver(kvm_vm_t *vm, uint64_t addr, uint32_t data)
{
	if ((addr & 0xFFF00000ULL) != 0xFEE00000ULL) return;
	kvm_lapic_deliver(vm, NULL, (uint32_t)(addr >> 12) & 0xFF, (addr >> 2) & 1, 0,
	                  (data >> 8) & 7, data & 0xFF, (data >> 15) & 1);
}

/* ── the timer ───────────────────────────────────────────── */

static uint64_t tick_ns(kvm_lapic_t *a)
{
	uint32_t d = *R(a, APIC_TDCR);
	uint32_t code = (d & 3) | ((d & 8) >> 1);
	return code == 7 ? 1 : 2ULL << code;   /* a 1 GHz bus divided */
}

static void timer_start(kvm_lapic_t *a, uint64_t now)
{
	uint32_t count = *R(a, APIC_TMICT);
	a->timer_end = count ? now + count * tick_ns(a) : 0;
	a->timer_period = count * tick_ns(a);
}

/* The deadline passed: the interrupt, and the next period. Vcpu thread. */
void kvm_lapic_timer_check(kvm_vcpu_t *v, uint64_t now)
{
	kvm_lapic_t *a = v->apic;
	if (!a || !a->timer_end || now < a->timer_end) return;
	uint32_t lvt = *R(a, APIC_LVTT);
	if (!(lvt & LVT_MASKED)) accept(v, lvt & 0xFF, 0);
	if ((lvt & LVT_PERIODIC) && a->timer_period) {
		a->timer_end += a->timer_period;
		if (a->timer_end <= now) a->timer_end = now + a->timer_period;   /* far behind: no storm of catch-ups */
	} else {
		a->timer_end = 0;
	}
}

/* ── interrupts for the guest ────────────────────────────── */

/* the highest vector the guest may take now, or -1 */
int kvm_lapic_pending(kvm_vcpu_t *v)
{
	kvm_lapic_t *a = v->apic;
	if (!a || !(*R(a, APIC_SVR) & SVR_ENABLE) || !(v->apic_base & (1ULL << 11))) return -1;
	int irrv = highest(a, APIC_IRR);
	if (irrv < 0 || ((uint32_t)irrv & 0xF0) <= (ppr(a) & 0xF0)) return -1;
	return irrv;
}

/* it was injected: in service from now */
void kvm_lapic_ack(kvm_vcpu_t *v, int vec)
{
	vbit_clear(v->apic, APIC_IRR, vec);
	vbit_set(v->apic, APIC_ISR, vec);
}

/* Whether an interrupt from the VMM's PIC gets through (LINT0 in ExtINT mode,
 * or the APIC off): the VMM injects those with KVM_INTERRUPT. */
int kvm_lapic_accepts_pic(kvm_vcpu_t *v)
{
	kvm_lapic_t *a = v->apic;
	if (!a || !(v->apic_base & (1ULL << 11))) return 1;
	uint32_t lvt0 = *R(a, APIC_LVT0);
	return !(lvt0 & LVT_MASKED) && ((lvt0 >> 8) & 7) == DM_EXTINT;
}

/* EOI: the highest in-service vector ends; a level-triggered one is returned,
 * for the VMM's IOAPIC (KVM_EXIT_IOAPIC_EOI), else -1. */
static int eoi(kvm_vcpu_t *v)
{
	kvm_lapic_t *a = v->apic;
	int vec = highest(a, APIC_ISR);
	if (vec < 0) return -1;
	vbit_clear(a, APIC_ISR, vec);
	return vbit(a, APIC_TMR, vec) ? vec : -1;
}

/* ── registers ───────────────────────────────────────────── */

uint32_t kvm_lapic_read(kvm_vcpu_t *v, uint32_t off)
{
	kvm_lapic_t *a = v->apic;
	off &= 0xFF0;
	switch (off) {
	case APIC_PPR: return ppr(a);
	case APIC_TMCCT: {
		uint64_t now = mono_ns();
		if (!a->timer_end || now >= a->timer_end) return 0;
		return (uint32_t)((a->timer_end - now) / tick_ns(a));
	}
	case APIC_EOI:
		return 0;
	default:
		return off < 0x400 ? __atomic_load_n(R(a, off), __ATOMIC_ACQUIRE) : 0;
	}
}

/* A guest store. Returns a level-triggered vector an EOI ended (for userspace),
 * else -1. */
int kvm_lapic_write(kvm_vcpu_t *v, uint32_t off, uint32_t val)
{
	kvm_lapic_t *a = v->apic;
	off &= 0xFF0;
	switch (off) {
	case APIC_ID:
		*R(a, off) = val & 0xFF000000;
		break;
	case APIC_TPR:
		*R(a, off) = val & 0xFF;
		break;
	case APIC_EOI:
		return eoi(v);
	case APIC_LDR:
		*R(a, off) = val & 0xFF000000;
		break;
	case APIC_DFR:
		*R(a, off) = val | 0x0FFFFFFF;
		break;
	case APIC_SVR:
		*R(a, off) = val & 0x3FF;
		if (!(val & SVR_ENABLE))   /* software-disabled: every LVT masked */
			for (uint32_t o = APIC_LVTT; o <= APIC_LVTERR; o += 0x10) *R(a, o) |= LVT_MASKED;
		break;
	case APIC_ESR:
		*R(a, off) = 0;
		break;
	case APIC_ICR2:
		*R(a, off) = val & 0xFF000000;
		break;
	case APIC_ICR:
		*R(a, off) = val & ~(1U << 12);   /* delivered at once: never pending */
		/* An INIT with the level bit clear is the "INIT level de-assert" that
		 * old APICs used to resynchronise arbitration IDs: it does nothing
		 * since the Pentium 4 (SDM 10.6.1), and Linux still sends it to
		 * everyone, itself included. */
		if (((val >> 8) & 7) == DM_INIT && !(val & (1U << 14))) break;
		kvm_lapic_deliver(v->vm, v, *R(a, APIC_ICR2) >> 24, (val >> 11) & 1, (val >> 18) & 3,
		                  (val >> 8) & 7, val & 0xFF, (val >> 15) & 1);
		break;
	case APIC_LVTT: case APIC_LVTTHM: case APIC_LVTPC: case APIC_LVT0: case APIC_LVT1: case APIC_LVTERR:
		if (!(*R(a, APIC_SVR) & SVR_ENABLE)) val |= LVT_MASKED;
		*R(a, off) = val & (off == APIC_LVTT ? 0x700FF : 0x1F7FF);
		break;
	case APIC_TMICT:
		*R(a, off) = val;
		timer_start(a, mono_ns());
		break;
	case APIC_TDCR:
		*R(a, off) = val & 0xB;
		break;
	default:
		break;   /* read-only, reserved */
	}
	return -1;
}

/* CR8 is TPR bits 7:4 */
uint64_t kvm_lapic_cr8(kvm_vcpu_t *v) { return (*R(v->apic, APIC_TPR) >> 4) & 15; }
void kvm_lapic_set_cr8(kvm_vcpu_t *v, uint64_t cr8) { *R(v->apic, APIC_TPR) = (uint32_t)(cr8 & 15) << 4; }

/* KVM_GET_LAPIC / KVM_SET_LAPIC: the register page as it is; a set timer
 * starts counting again from the moment of the set */
void kvm_lapic_get(kvm_vcpu_t *v, struct kvm_lapic_state *s)
{
	memcpy(s->regs, v->apic->reg, sizeof(s->regs));
	*(uint32_t *)&s->regs[APIC_PPR] = ppr(v->apic);
	*(uint32_t *)&s->regs[APIC_TMCCT] = kvm_lapic_read(v, APIC_TMCCT);
}

void kvm_lapic_set(kvm_vcpu_t *v, const struct kvm_lapic_state *s)
{
	memcpy(v->apic->reg, s->regs, sizeof(v->apic->reg));
	timer_start(v->apic, mono_ns());
}
