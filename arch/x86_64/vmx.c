/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Intel VT-x for KVM: VMX operation per CPU, the VMCS of each vcpu, and the
 * loop that runs a guest until something needs the VMM (userspace).
 *
 * VMX operation. A CPU enters it (VMXON) the first time a vcpu runs there and
 * stays in it: leaving and entering again would make a hypervisor under us
 * drop its cached state of our guests each time, which is what makes nested
 * KVM slow. A CPU back from S3 is reset, so vmx_cpu_reset forgets it.
 *
 * The VMCS. A vcpu's VMCS is current on a CPU only while its thread runs in
 * KVM_RUN (or one GET/SET ioctl) with interrupts off, and is cleared (VMCLEAR)
 * before interrupts come back on for good. The kernel does not preempt kernel
 * code, so between the two the thread cannot move; after VMCLEAR it may run
 * anywhere next time. That is why there is no per-CPU "last vcpu" state.
 *
 * Running (vmx_vcpu_run). The inner loop (run_loaded) keeps the VMCS loaded and
 * interrupts off: it enters the guest, handles the exits that need neither
 * sleep nor userspace (CPUID, control registers, an interrupt for the host),
 * and enters again. An exit that needs a page mapped, or a host that wants the
 * CPU back, leaves to the outer loop, which may sleep; port I/O, HLT and
 * failures go to userspace through struct kvm_run, as Linux's KVM does.
 *
 * The guest's x87/SSE/AVX registers are loaded at the start of the inner loop
 * and saved at its end. Nothing else in the kernel uses those registers (it is
 * built without SSE), and every entry from user mode or an interrupt saves and
 * restores the interrupted context's own, so the guest's survive an interrupt
 * taken between two exits.
 *
 * Provenance: VMX capability MSRs, the VMCS layout and its checks, exit
 * qualifications and the guest/host state rules are from the Intel SDM Vol. 3
 * (chapters 23-28, appendix A-C). The KVM-visible behaviour (reset state, what
 * goes to userspace, kvm_run fields) follows Linux's public KVM API description
 * (Documentation/virt/kvm/api.rst). FreeBSD's vmm (sys/amd64/vmm/intel, BSD)
 * was the model for the control-MSR adjustment. No Linux source was read. */

#include <asm/kvm.h>
#include <asm/vmx.h>
#include <asm/cpu.h>
#include <asm/gdt.h>
#include <uapi/kvm.h>
#include <kernel/hal.h>
#include <kernel/irq.h>
#include <kernel/proc.h>
#include <kernel/smp.h>
#include <kernel/kprintf.h>
#include <kernel/time.h>
#include <kernel/errno.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>

#define CR0_PE (1ULL << 0)
#define CR0_TS (1ULL << 3)
#define CR0_NW (1ULL << 29)
#define CR0_CD (1ULL << 30)
#define CR0_PG (1ULL << 31)
#define CR4_PAE     (1ULL << 5)
#define RFLAGS_IF   (1ULL << 9)

/* inner-loop results */
#define RUN_GUEST 1   /* enter the guest again */
#define RUN_OUTER 2   /* leave the inner loop, do the slow work, come back */
#define RUN_USER  0   /* return to userspace, kvm_run says why */

#define NO_PREFAULT (~0ULL)

static struct {
	uint32_t rev;                     /* VMCS revision identifier */
	uint32_t pin, cpu, cpu2, exit, entry;
	int      preempt;               /* the VMX preemption timer is there */
	uint32_t preempt_shift;
	uint64_t cr0_fixed0, cr0_fixed1, cr4_fixed0, cr4_fixed1;
} vmx;

/* per CPU: the VMXON region and whether the CPU is in VMX operation */
static uint64_t vmxon_page[MAX_CPUS];
static uint8_t  vmx_live[MAX_CPUS];

static int this_cpu(void) { return (int)arch_get_cpu_ctx()->cpu_id; }

static uint64_t read_cr0(void) { uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static uint64_t read_cr4(void) { uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
	__asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

static void xsetbv(uint64_t v)
{
	__asm__ volatile("xsetbv" :: "c"(0), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/* A control field from its capability MSR: bits the CPU forces on are on,
 * need must all be allowed, want is taken where allowed. */
static int ctl_adjust(uint32_t msr, uint32_t need, uint32_t want, uint32_t *out)
{
	uint64_t cap = vmx_rdmsr(msr);
	uint32_t must = (uint32_t)cap, may = (uint32_t)(cap >> 32);
	if (need & ~may) return -1;
	*out = must | need | (want & may);
	return 0;
}

int vmx_probe(void)
{
	uint32_t r[4];
	cpuid(1, 0, r);
	if (!(r[2] & (1U << 5))) return -ENODEV;   /* no VMX */
	uint64_t fc = vmx_rdmsr(MSR_IA32_FEATURE_CONTROL);
	if ((fc & FEATURE_CONTROL_LOCK) && !(fc & FEATURE_CONTROL_VMX)) {
		kprintf(KERN_INFO "kvm: VT-x is turned off in the firmware\n");
		return -ENODEV;
	}
	uint64_t basic = vmx_rdmsr(MSR_IA32_VMX_BASIC);
	if (((basic >> 50) & 0xF) != 6) return -ENODEV;   /* VMCS must be write-back memory */
	vmx.rev = (uint32_t)basic & 0x7FFFFFFF;
	int tru = (basic & VMX_BASIC_TRUE_CTLS) != 0;

	if (ctl_adjust(tru ? MSR_IA32_VMX_TRUE_PINBASED : MSR_IA32_VMX_PINBASED,
	               PIN_EXT_INTR_EXIT | PIN_NMI_EXIT | PIN_VIRTUAL_NMIS, PIN_PREEMPT_TIMER, &vmx.pin) ||
	    ctl_adjust(tru ? MSR_IA32_VMX_TRUE_PROCBASED : MSR_IA32_VMX_PROCBASED,
	               CPU_HLT_EXIT | CPU_UNCOND_IO_EXIT | CPU_MWAIT_EXIT | CPU_MONITOR_EXIT |
	               CPU_CR8_LOAD_EXIT | CPU_CR8_STORE_EXIT | CPU_MOV_DR_EXIT | CPU_TSC_OFFSET |
	               CPU_SECONDARY, 0, &vmx.cpu) ||
	    ctl_adjust(MSR_IA32_VMX_PROCBASED2, CPU2_EPT | CPU2_UNRESTRICTED_GUEST, 0, &vmx.cpu2) ||
	    ctl_adjust(tru ? MSR_IA32_VMX_TRUE_EXIT : MSR_IA32_VMX_EXIT,
	               EXIT_HOST_ADDR64 | EXIT_SAVE_EFER | EXIT_LOAD_EFER | EXIT_SAVE_PAT | EXIT_LOAD_PAT,
	               0, &vmx.exit) ||
	    ctl_adjust(tru ? MSR_IA32_VMX_TRUE_ENTRY : MSR_IA32_VMX_ENTRY,
	               ENTRY_LOAD_EFER | ENTRY_LOAD_PAT, 0, &vmx.entry)) {
		kprintf(KERN_INFO "kvm: VT-x lacks a control this KVM needs (EPT, unrestricted guest, virtual NMIs, EFER/PAT switch)\n");
		return -ENODEV;
	}
	/* No TPR shadow: a guest's CR8 is the VMM's, through the CR8 exits. A CPU
	 * that forced it on would want a virtual-APIC page. */
	if (vmx.cpu & CPU_TPR_SHADOW) return -ENODEV;
	vmx.entry &= ~ENTRY_IA32E_GUEST;   /* set per vcpu from its EFER.LMA */
	vmx.cpu &= ~(CPU_INTR_WINDOW_EXIT | CPU_NMI_WINDOW_EXIT);   /* per vcpu, when an event waits */
	/* the preemption timer stops a guest at its APIC timer's deadline; it
	 * counts TSC ticks shifted right by IA32_VMX_MISC[4:0] */
	vmx.preempt = (vmx.pin & PIN_PREEMPT_TIMER) != 0;
	vmx.pin &= ~PIN_PREEMPT_TIMER;
	vmx.preempt_shift = (uint32_t)(vmx_rdmsr(MSR_IA32_VMX_MISC) & 0x1F);

	uint64_t ept = vmx_rdmsr(MSR_IA32_VMX_EPT_VPID_CAP);
	uint64_t ept_need = EPT_CAP_WALK4 | EPT_CAP_WB | EPT_CAP_INVEPT | EPT_CAP_INVEPT_SINGLE;
	if ((ept & ept_need) != ept_need) {
		kprintf(KERN_INFO "kvm: EPT lacks 4-level walks, write-back or single-context INVEPT\n");
		return -ENODEV;
	}
	vmx.cr0_fixed0 = vmx_rdmsr(MSR_IA32_VMX_CR0_FIXED0);
	vmx.cr0_fixed1 = vmx_rdmsr(MSR_IA32_VMX_CR0_FIXED1);
	vmx.cr4_fixed0 = vmx_rdmsr(MSR_IA32_VMX_CR4_FIXED0);
	vmx.cr4_fixed1 = vmx_rdmsr(MSR_IA32_VMX_CR4_FIXED1);

	/* the VMXON regions now, while allocating may still sleep */
	for (int i = 0; i < smp_ncpus && i < MAX_CPUS; i++) {
		uint64_t pg = (uint64_t)buddy_alloc(0);
		if (!pg) return -ENOMEM;
		memset((void *)PHYS_TO_VIRT(pg), 0, PAGE_SIZE);
		*(uint32_t *)PHYS_TO_VIRT(pg) = vmx.rev;
		vmxon_page[i] = pg;
	}
	return 0;
}

void vmx_cpu_reset(int cpu)
{
	if (cpu >= 0 && cpu < MAX_CPUS) vmx_live[cpu] = 0;
}

/* Put this CPU in VMX operation if it is not yet. Interrupts are off. */
static int vmx_cpu_on(int cpu)
{
	if (vmx_live[cpu]) return 0;
	if (cpu >= MAX_CPUS || !vmxon_page[cpu]) return -ENODEV;
	uint64_t fc = vmx_rdmsr(MSR_IA32_FEATURE_CONTROL);
	if (!(fc & FEATURE_CONTROL_LOCK))
		vmx_wrmsr(MSR_IA32_FEATURE_CONTROL, fc | FEATURE_CONTROL_VMX | FEATURE_CONTROL_LOCK);
	else if (!(fc & FEATURE_CONTROL_VMX))
		return -ENODEV;
	/* VMX operation pins some CR0 and CR4 bits (CR0.NE, CR4.VMXE) */
	uint64_t cr0 = (read_cr0() | vmx.cr0_fixed0) & vmx.cr0_fixed1;
	__asm__ volatile("mov %0, %%cr0" :: "r"(cr0));
	uint64_t cr4_old = read_cr4();
	uint64_t cr4 = (cr4_old | vmx.cr4_fixed0) & vmx.cr4_fixed1;
	__asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
	if (vmx_on(vmxon_page[cpu])) {
		__asm__ volatile("mov %0, %%cr4" :: "r"(cr4_old));
		kprintf(KERN_ERR "kvm: VMXON failed on cpu %d\n", cpu);
		return -EIO;
	}
	vmx_live[cpu] = 1;
	return 0;
}

/* Make v's VMCS current on this CPU: interrupts go off and stay off until
 * vmcs_put. Returns the CPU, or a negative errno with interrupts as they were. */
int vmcs_load(kvm_vcpu_t *v, uint64_t *irqf)
{
	*irqf = irq_save();
	int cpu = this_cpu();
	int r = vmx_cpu_on(cpu);
	if (!r && vmx_ptrld(v->vmcs)) r = -EIO;
	if (r) { irq_restore(*irqf); return r; }
	return cpu;
}

void vmcs_put(kvm_vcpu_t *v, uint64_t irqf)
{
	vmx_clear(v->vmcs);
	irq_restore(irqf);
}

/* ── guest state helpers (VMCS current) ───────────────────── */

static void seg_write(int i, uint16_t sel, uint64_t base, uint32_t limit, uint32_t ar)
{
	vmwrite(GUEST_ES_SELECTOR + 2 * i, sel);
	vmwrite(GUEST_ES_BASE + 2 * i, base);
	vmwrite(GUEST_ES_LIMIT + 2 * i, limit);
	vmwrite(GUEST_ES_AR + 2 * i, ar);
}

void vmx_seg_set(int i, const struct kvm_segment *s)
{
	uint32_t ar;
	if (s->unusable || !s->present)
		ar = AR_UNUSABLE;
	else
		ar = (s->type & 15) | (s->s & 1) << 4 | (s->dpl & 3) << 5 | 1U << 7 |
		     (s->avl & 1) << 12 | (s->l & 1) << 13 | (s->db & 1) << 14 | (s->g & 1) << 15;
	seg_write(i, s->selector, s->base, s->limit, ar);
}

void vmx_seg_get(int i, struct kvm_segment *s)
{
	uint32_t ar = (uint32_t)vmread(GUEST_ES_AR + 2 * i);
	memset(s, 0, sizeof(*s));
	s->selector = (uint16_t)vmread(GUEST_ES_SELECTOR + 2 * i);
	s->base     = vmread(GUEST_ES_BASE + 2 * i);
	s->limit    = (uint32_t)vmread(GUEST_ES_LIMIT + 2 * i);
	s->type     = ar & 15;
	s->s        = (ar >> 4) & 1;
	s->dpl      = (ar >> 5) & 3;
	s->avl      = (ar >> 12) & 1;
	s->l        = (ar >> 13) & 1;
	s->db       = (ar >> 14) & 1;
	s->g        = (ar >> 15) & 1;
	s->unusable = (ar >> 16) & 1;
	s->present  = s->unusable ? 0 : (ar >> 7) & 1;
}

/* EFER and the entry control that must agree with its LMA bit */
void vmx_efer_update(kvm_vcpu_t *v)
{
	vmwrite(GUEST_IA32_EFER, v->efer);
	uint32_t entry = vmx.entry | ((v->efer & EFER_LMA) ? ENTRY_IA32E_GUEST : 0);
	vmwrite(VM_ENTRY_CONTROLS, entry);
}

/* The guest's CR0 is what it wrote (the read shadow shows it back); the CPU
 * runs with the bits VMX operation fixes, and with caching on: CD and NW stay
 * the VMM's, as on Linux's KVM. PE and PG are free under unrestricted guest. */
void vmx_cr0_apply(kvm_vcpu_t *v)
{
	uint64_t hw = (v->cr0 | (vmx.cr0_fixed0 & ~(CR0_PE | CR0_PG))) & vmx.cr0_fixed1;
	hw &= ~(CR0_CD | CR0_NW);
	vmwrite(GUEST_CR0, hw);
	vmwrite(CR0_READ_SHADOW, v->cr0);
}

void vmx_cr4_apply(kvm_vcpu_t *v)
{
	vmwrite(GUEST_CR4, (v->cr4 | vmx.cr4_fixed0) & vmx.cr4_fixed1);
	vmwrite(CR4_READ_SHADOW, v->cr4);
}

/* A guest write to CR0: turning paging on with EFER.LME set enters long mode,
 * turning it off leaves it. */
static void cr0_write(kvm_vcpu_t *v, uint64_t val)
{
	v->cr0 = val;
	if ((val & CR0_PG) && (v->efer & EFER_LME)) v->efer |= EFER_LMA;
	if (!(val & CR0_PG)) v->efer &= ~EFER_LMA;
	vmx_cr0_apply(v);
	vmx_efer_update(v);
}

/* PAE paging outside long mode takes its four PDPTEs from the VMCS, not from
 * memory: load them when CR0, CR3 or CR4 changes. -EAGAIN when the guest page
 * holding them is not mapped yet (v->prefault names it). */
int vmx_pdptrs_load(kvm_vcpu_t *v)
{
	if (!(v->cr0 & CR0_PG) || !(v->cr4 & CR4_PAE) || (v->efer & EFER_LMA)) return 0;
	uint64_t pdpte[4];
	int r = kvm_gpa_read(v->vm, vmread(GUEST_CR3) & ~0x1FULL, pdpte, sizeof(pdpte), &v->prefault);
	if (r) return r;
	for (int i = 0; i < 4; i++) vmwrite(GUEST_PDPTE0 + 2 * i, pdpte[i]);
	return 0;
}

static void host_state(void)
{
	struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdtr, idtr;
	__asm__ volatile("sgdt %0" : "=m"(gdtr));
	__asm__ volatile("sidt %0" : "=m"(idtr));
	const uint8_t *d = (const uint8_t *)(gdtr.base + GDT_TSS);
	uint64_t tr = d[2] | (uint64_t)d[3] << 8 | (uint64_t)d[4] << 16 | (uint64_t)d[7] << 24 |
	              (uint64_t)(*(const uint32_t *)(d + 8)) << 32;
	vmwrite(HOST_CR0, read_cr0());
	vmwrite(HOST_CR3, read_cr3());
	vmwrite(HOST_CR4, read_cr4());
	vmwrite(HOST_FS_BASE, vmx_rdmsr(MSR_FS_BASE_));
	vmwrite(HOST_GS_BASE, vmx_rdmsr(MSR_GS_BASE_));
	vmwrite(HOST_TR_BASE, tr);
	vmwrite(HOST_GDTR_BASE, gdtr.base);
	vmwrite(HOST_IDTR_BASE, idtr.base);
	vmwrite(HOST_IA32_SYSENTER_CS, vmx_rdmsr(MSR_IA32_SYSENTER_CS));
	vmwrite(HOST_IA32_SYSENTER_ESP, vmx_rdmsr(MSR_IA32_SYSENTER_ESP));
	vmwrite(HOST_IA32_SYSENTER_EIP, vmx_rdmsr(MSR_IA32_SYSENTER_EIP));
	vmwrite(HOST_IA32_EFER, vmx_rdmsr(MSR_EFER));
	vmwrite(HOST_IA32_PAT, vmx_rdmsr(MSR_IA32_PAT));
}

static void guest_reset(kvm_vcpu_t *v);

/* Controls and the parts of the host state that never change, then the
 * reset state */
static void vmcs_setup(kvm_vcpu_t *v)
{
	vmwrite(PIN_BASED_CONTROLS, vmx.pin);
	vmwrite(CPU_BASED_CONTROLS, vmx.cpu);
	vmwrite(SECONDARY_CONTROLS, vmx.cpu2);
	vmwrite(VM_EXIT_CONTROLS, vmx.exit);
	vmwrite(VM_ENTRY_CONTROLS, vmx.entry);
	vmwrite(EXCEPTION_BITMAP, 0);
	vmwrite(PF_ERROR_CODE_MASK, 0);
	vmwrite(PF_ERROR_CODE_MATCH, 0);
	vmwrite(CR3_TARGET_COUNT, 0);
	/* the switched MSRs: guest values loaded on entry and stored back on
	 * exit in one area, the host's loaded on exit from the other half */
	vmwrite(VM_ENTRY_MSR_LOAD_ADDR, v->msr_page);
	vmwrite(VM_EXIT_MSR_STORE_ADDR, v->msr_page);
	vmwrite(VM_EXIT_MSR_LOAD_ADDR, v->msr_page + PAGE_SIZE / 2);
	vmwrite(VM_EXIT_MSR_STORE_COUNT, 0);
	vmwrite(VM_EXIT_MSR_LOAD_COUNT, 0);
	vmwrite(VM_ENTRY_MSR_LOAD_COUNT, 0);
	v->msr_count = 0;
	v->cpu_ctl = vmx.cpu;
	v->pin_ctl = vmx.pin;
	vmwrite(VM_ENTRY_INTR_INFO, 0);
	vmwrite(EPT_POINTER, v->vm->eptp);
	vmwrite(VMCS_LINK_POINTER, ~0ULL);
	vmwrite(TSC_OFFSET, 0);
	/* CR0: everything but TS exits when the guest changes it; CR4: all of it */
	vmwrite(CR0_GUEST_HOST_MASK, ~CR0_TS);
	vmwrite(CR4_GUEST_HOST_MASK, ~0ULL);

	vmwrite(HOST_CS_SELECTOR, GDT_KCODE);
	vmwrite(HOST_SS_SELECTOR, GDT_KDATA);
	vmwrite(HOST_DS_SELECTOR, GDT_KDATA);
	vmwrite(HOST_ES_SELECTOR, GDT_KDATA);
	vmwrite(HOST_FS_SELECTOR, 0);
	vmwrite(HOST_GS_SELECTOR, 0);
	vmwrite(HOST_TR_SELECTOR, GDT_TSS);
	vmwrite(HOST_RIP, (uint64_t)vmx_exit_entry);

	guest_reset(v);
}

/* The architectural reset state (Intel SDM 9.1.1, as Linux's KVM presents it:
 * CS f000:fff0 with base ffff0000, real mode, caching off). Power-on and INIT. */
static void guest_reset(kvm_vcpu_t *v)
{
	seg_write(SEG_CS, 0xF000, 0xFFFF0000, 0xFFFF, 0x9B);   /* code, accessed, present */
	seg_write(SEG_DS, 0, 0, 0xFFFF, 0x93);                 /* data, writable, accessed */
	seg_write(SEG_ES, 0, 0, 0xFFFF, 0x93);
	seg_write(SEG_FS, 0, 0, 0xFFFF, 0x93);
	seg_write(SEG_GS, 0, 0, 0xFFFF, 0x93);
	seg_write(SEG_SS, 0, 0, 0xFFFF, 0x93);
	seg_write(SEG_LDTR, 0, 0, 0xFFFF, 0x82);               /* LDT, present */
	seg_write(SEG_TR, 0, 0, 0xFFFF, 0x8B);                 /* busy TSS, present */
	vmwrite(GUEST_GDTR_BASE, 0);
	vmwrite(GUEST_GDTR_LIMIT, 0xFFFF);
	vmwrite(GUEST_IDTR_BASE, 0);
	vmwrite(GUEST_IDTR_LIMIT, 0xFFFF);
	vmwrite(GUEST_RIP, 0xFFF0);
	vmwrite(GUEST_RSP, 0);
	vmwrite(GUEST_RFLAGS, 0x2);
	vmwrite(GUEST_CR3, 0);
	vmwrite(GUEST_DR7, 0x400);
	vmwrite(GUEST_IA32_DEBUGCTL, 0);
	vmwrite(GUEST_IA32_PAT, 0x0007040600070406ULL);   /* the power-on PAT */
	vmwrite(GUEST_SYSENTER_CS, 0);
	vmwrite(GUEST_SYSENTER_ESP, 0);
	vmwrite(GUEST_SYSENTER_EIP, 0);
	vmwrite(GUEST_INTERRUPTIBILITY, 0);
	vmwrite(GUEST_ACTIVITY_STATE, 0);
	vmwrite(GUEST_PENDING_DBG, 0);
	v->cr0 = 0x60000010;   /* CD, NW, ET */
	v->cr4 = 0;
	v->efer = 0;
	vmx_cr0_apply(v);
	vmx_cr4_apply(v);
	vmx_efer_update(v);
	memset(v->g.r, 0, sizeof(v->g.r));
	v->g.r[R_DX] = 0x600;   /* the reset value of EDX: family 6 */
	v->g.cr2 = 0;
	v->xcr0 = 1;
	vmwrite(VM_ENTRY_INTR_INFO, 0);
}

/* INIT and SIPI from another vcpu's APIC: the reset, then the start at
 * vector:0000 in real mode (SDM 8.4.4). VMCS loaded. */
static void apic_events(kvm_vcpu_t *v)
{
	if (v->do_reset) {
		v->do_reset = 0;
		guest_reset(v);
		uint64_t base = v->apic_base;
		kvm_lapic_reset(v);
		v->apic_base = base;
	}
	if (v->do_sipi) {
		v->do_sipi = 0;
		uint32_t vec = (uint32_t)v->sipi_vector & 0xFF;
		seg_write(SEG_CS, (uint16_t)(vec << 8), (uint64_t)vec << 12, 0xFFFF, 0x9B);
		vmwrite(GUEST_RIP, 0);
	}
}

int vmx_vcpu_init(kvm_vcpu_t *v)
{
	uint64_t vmcs = (uint64_t)buddy_alloc(0);
	uint64_t fpu = (uint64_t)buddy_alloc(0);
	uint64_t msr = (uint64_t)buddy_alloc(0);
	if (!vmcs || !fpu || !msr) goto nomem;
	memset((void *)PHYS_TO_VIRT(msr), 0, PAGE_SIZE);
	v->msr_page = msr;
	v->msr_guest = (struct kvm_msr_entry *)PHYS_TO_VIRT(msr);
	v->msr_host = (struct kvm_msr_entry *)PHYS_TO_VIRT(msr + PAGE_SIZE / 2);
	kvm_msr_reset(v);
	v->irq_pending = -1;
	memset((void *)PHYS_TO_VIRT(vmcs), 0, PAGE_SIZE);
	*(uint32_t *)PHYS_TO_VIRT(vmcs) = vmx.rev;
	v->vmcs = vmcs;
	v->fpu = (uint8_t *)PHYS_TO_VIRT(fpu);
	memset(v->fpu, 0, PAGE_SIZE);
	*(uint16_t *)&v->fpu[0]  = 0x037F;   /* FCW, as after FNINIT */
	*(uint32_t *)&v->fpu[24] = 0x1F80;   /* MXCSR */
	v->xcr0 = 1;
	v->apic_base = 0xFEE00000ULL | (1ULL << 11) | (v->id == 0 ? 1ULL << 8 : 0);   /* enabled, BSP */
	v->prefault = NO_PREFAULT;
	v->guest_cpu = -1;

	uint64_t f;
	/* VMCLEAR first: it puts the new VMCS in the clear launch state */
	f = irq_save();
	int r = vmx_cpu_on(this_cpu());
	if (!r && vmx_clear(vmcs)) r = -EIO;
	irq_restore(f);
	if (r) goto fail;
	r = vmcs_load(v, &f);
	if (r < 0) goto fail;
	vmcs_setup(v);
	vmcs_put(v, f);
	return 0;
nomem:
	r = -ENOMEM;
fail:
	if (vmcs) buddy_free(vmcs, 0);
	if (fpu) buddy_free(fpu, 0);
	if (msr) buddy_free(msr, 0);
	v->vmcs = 0;
	v->fpu = NULL;
	v->msr_page = 0;
	return r;
}

void vmx_vcpu_free(kvm_vcpu_t *v)
{
	/* never current anywhere: every load ends in VMCLEAR */
	if (v->vmcs) buddy_free(v->vmcs, 0);
	if (v->fpu) buddy_free(VIRT_TO_PHYS(v->fpu), 0);
	if (v->msr_page) buddy_free(v->msr_page, 0);
	kfree(v->cpuid);
	v->vmcs = 0;
	v->fpu = NULL;
	v->msr_page = 0;
	v->cpuid = NULL;
}

/* ── exits ────────────────────────────────────────────────── */

static void skip_instruction(void)
{
	vmwrite(GUEST_RIP, vmread(GUEST_RIP) + vmread(VM_EXIT_INSTRUCTION_LEN));
	/* the instruction completed: a blocking it started (STI, MOV SS) is over */
	uint64_t is = vmread(GUEST_INTERRUPTIBILITY);
	if (is & (INTERRUPTIBILITY_STI | INTERRUPTIBILITY_MOVSS))
		vmwrite(GUEST_INTERRUPTIBILITY, is & ~(uint64_t)(INTERRUPTIBILITY_STI | INTERRUPTIBILITY_MOVSS));
}

static uint64_t gpr_get(kvm_vcpu_t *v, int reg) { return reg == R_SP ? vmread(GUEST_RSP) : v->g.r[reg]; }

static void gpr_set(kvm_vcpu_t *v, int reg, uint64_t val)
{
	if (reg == R_SP) vmwrite(GUEST_RSP, val);
	else v->g.r[reg] = val;
}

/* A hardware exception for the guest at its next entry. Real mode has no
 * error codes; VM entry refuses one there. */
static void inject_exception(kvm_vcpu_t *v, int vector, int has_err, uint32_t err)
{
	uint32_t info = INTR_INFO_VALID | INTR_TYPE_HW_EXC | (uint32_t)vector;
	if (has_err && (v->cr0 & CR0_PE)) {
		info |= INTR_INFO_ERRCODE;
		vmwrite(VM_ENTRY_EXCEPTION_ERROR_CODE, err);
	}
	vmwrite(VM_ENTRY_INTR_INFO, info);
}

static int internal_error(kvm_vcpu_t *v, uint32_t suberror, uint64_t d0, uint64_t d1)
{
	struct kvm_run *run = v->run;
	run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
	run->internal.suberror = suberror;
	run->internal.ndata = 2;
	run->internal.data[0] = d0;
	run->internal.data[1] = d1;
	return RUN_USER;
}

/* Port I/O goes to userspace. OUT data is copied out now; an IN finishes at
 * the next KVM_RUN (pio_finish), when userspace has written the data. The
 * instruction is complete either way, as Linux's KVM leaves it. */
static int exit_io(kvm_vcpu_t *v)
{
	uint64_t q = vmread(EXIT_QUALIFICATION);
	uint32_t size = (uint32_t)(q & 7) + 1;
	int in = (q >> 3) & 1, string = (q >> 4) & 1;
	if (size != 1 && size != 2 && size != 4)
		return internal_error(v, KVM_INTERNAL_ERROR_EMULATION, EXIT_IO, q);
	if (string) {
		switch (kvm_pio_string(v, q)) {
		case KVM_MMIO_GUEST: return RUN_GUEST;
		case KVM_MMIO_RETRY: return RUN_OUTER;
		case KVM_MMIO_USER:  return RUN_USER;
		default: return internal_error(v, KVM_INTERNAL_ERROR_EMULATION, EXIT_IO, q);
		}
	}
	if (!in && kvm_ioeventfd_hit(v->vm, 1, q >> 16, size, v->g.r[R_AX])) {
		skip_instruction();
		return RUN_GUEST;
	}
	struct kvm_run *run = v->run;
	run->exit_reason = KVM_EXIT_IO;
	run->io.direction = in ? KVM_EXIT_IO_IN : KVM_EXIT_IO_OUT;
	run->io.size = (uint8_t)size;
	run->io.port = (uint16_t)(q >> 16);
	run->io.count = 1;
	run->io.data_offset = PAGE_SIZE;
	if (in) v->pio_in = size;
	else memcpy((void *)PHYS_TO_VIRT(v->pio_page), &v->g.r[R_AX], size);
	skip_instruction();
	return RUN_USER;
}

static void pio_finish(kvm_vcpu_t *v)
{
	uint32_t size = v->pio_in;
	uint64_t val = 0, *ax = &v->g.r[R_AX];
	if (!size) return;
	memcpy(&val, (void *)PHYS_TO_VIRT(v->pio_page), size);
	if (size == 4) *ax = val;   /* a 32-bit destination clears the upper half */
	else if (size == 2) *ax = (*ax & ~0xFFFFULL) | val;
	else *ax = (*ax & ~0xFFULL) | val;
	v->pio_in = 0;
}

static int exit_cpuid(kvm_vcpu_t *v)
{
	uint32_t r[4];
	kvm_cpuid_guest(v, (uint32_t)v->g.r[R_AX], (uint32_t)v->g.r[R_CX], r);
	v->g.r[R_AX] = r[0];
	v->g.r[R_BX] = r[1];
	v->g.r[R_CX] = r[2];
	v->g.r[R_DX] = r[3];
	skip_instruction();
	return RUN_GUEST;
}

/* MOV to/from CR0, CR3, CR4, CR8, CLTS and LMSW (only the bits the guest does
 * not own exit). Exit qualification: SDM table 27-3. */
static int exit_cr(kvm_vcpu_t *v)
{
	uint64_t q = vmread(EXIT_QUALIFICATION);
	int cr = q & 15, type = (q >> 4) & 3, reg = (q >> 8) & 15;
	if (type == 0) {
		uint64_t val = gpr_get(v, reg);
		switch (cr) {
		case 0: cr0_write(v, val); break;
		case 3: vmwrite(GUEST_CR3, val); break;
		case 4: v->cr4 = val; vmx_cr4_apply(v); break;
		case 8: {
			if (v->apic) { kvm_lapic_set_cr8(v, val); break; }
			uint64_t old = v->run->cr8;
			v->run->cr8 = val & 15;
			if (v->run->cr8 < old) {
				/* a lower TPR may unmask an interrupt the VMM holds */
				skip_instruction();
				v->run->exit_reason = KVM_EXIT_SET_TPR;
				return RUN_USER;
			}
			break;
		}
		default: return internal_error(v, KVM_INTERNAL_ERROR_EMULATION, EXIT_CR_ACCESS, q);
		}
		if (cr != 8) {
			int r = vmx_pdptrs_load(v);
			if (r == -EAGAIN) return RUN_OUTER;   /* map the PDPT, run the write again */
			if (r) { inject_exception(v, 13, 1, 0); return RUN_GUEST; }
		}
	} else if (type == 1) {
		if (cr == 3) gpr_set(v, reg, vmread(GUEST_CR3));
		else if (cr == 8) gpr_set(v, reg, v->apic ? kvm_lapic_cr8(v) : v->run->cr8);
		else return internal_error(v, KVM_INTERNAL_ERROR_EMULATION, EXIT_CR_ACCESS, q);
	} else if (type == 2) {
		cr0_write(v, v->cr0 & ~CR0_TS);
	} else {
		/* LMSW writes CR0[3:0] but cannot clear PE */
		uint64_t src = (q >> 16) & 0xF;
		cr0_write(v, (v->cr0 & ~0xEULL) | src | (v->cr0 & CR0_PE));
	}
	skip_instruction();
	return RUN_GUEST;
}

/* XSETBV: XCR0 is loaded for the guest around each entry (vmx_set_xcr0 says
 * which values are allowed) */
static int exit_xsetbv(kvm_vcpu_t *v)
{
	uint64_t val = (v->g.r[R_DX] << 32) | (uint32_t)v->g.r[R_AX];
	if ((uint32_t)v->g.r[R_CX] != 0 || vmx_set_xcr0(v, val)) {
		inject_exception(v, 13, 1, 0);
		return RUN_GUEST;
	}
	skip_instruction();
	return RUN_GUEST;
}

/* An event the CPU was delivering to the guest when the exit came (IDT
 * vectoring) is lost unless it is injected again. */
static void requeue_vectoring(void)
{
	uint32_t info = (uint32_t)vmread(IDT_VECTORING_INFO);
	if (!(info & INTR_INFO_VALID)) return;
	vmwrite(VM_ENTRY_INTR_INFO, info & ~(1U << 12));   /* bit 12 is not part of the entry format */
	if (info & INTR_INFO_ERRCODE)
		vmwrite(VM_ENTRY_EXCEPTION_ERROR_CODE, vmread(IDT_VECTORING_ERROR_CODE));
	uint32_t type = info & INTR_TYPE_MASK;
	if (type == INTR_TYPE_SW_INT || type == (5U << 8) || type == (6U << 8))
		vmwrite(VM_ENTRY_INSTRUCTION_LEN, vmread(VM_EXIT_INSTRUCTION_LEN));
}

static int host_wants_cpu(void)
{
	return arch_get_cpu_ctx()->need_resched || task_has_actionable_signal();
}

static int fail_entry(kvm_vcpu_t *v, uint64_t why, int cpu)
{
	static int told;
	if (told < 5) {
		told++;
		kprintf(KERN_WARN "kvm: VM entry failed (0x%lx): rip=0x%lx cr0=0x%lx cr4=0x%lx efer=0x%lx cs.ar=0x%lx\n",
		        (unsigned long)why, (unsigned long)vmread(GUEST_RIP), (unsigned long)vmread(GUEST_CR0),
		        (unsigned long)vmread(GUEST_CR4), (unsigned long)vmread(GUEST_IA32_EFER),
		        (unsigned long)vmread(GUEST_ES_AR + 2 * SEG_CS));
	}
	v->run->exit_reason = KVM_EXIT_FAIL_ENTRY;
	v->run->fail_entry.hardware_entry_failure_reason = why;
	v->run->fail_entry.cpu = (uint32_t)cpu;
	return RUN_USER;
}

/* RDMSR/WRMSR: kvm_msr.c knows the MSRs; any other is a #GP, as on hardware */
static int exit_msr(kvm_vcpu_t *v, int write)
{
	uint32_t idx = (uint32_t)v->g.r[R_CX];
	if (write) {
		uint64_t val = (v->g.r[R_DX] << 32) | (uint32_t)v->g.r[R_AX];
		if (kvm_msr_set(v, idx, val)) { inject_exception(v, 13, 1, 0); return RUN_GUEST; }
	} else {
		uint64_t val;
		if (kvm_msr_get(v, idx, &val)) { inject_exception(v, 13, 1, 0); return RUN_GUEST; }
		v->g.r[R_AX] = (uint32_t)val;
		v->g.r[R_DX] = val >> 32;
	}
	skip_instruction();
	return RUN_GUEST;
}

/* MOV to or from a debug register. The guest's DR0-3 and DR6 are only kept
 * here, never loaded: they are the host's registers too, and guest hardware
 * breakpoints are not worth switching them. DR7 is a VMCS field, so the host's
 * comes back on every exit. DR4/DR5 alias DR6/DR7 while CR4.DE is clear. */
static int exit_dr(kvm_vcpu_t *v)
{
	uint64_t q = vmread(EXIT_QUALIFICATION);
	int dr = q & 7, from = (q >> 4) & 1, reg = (q >> 8) & 15;
	if ((dr == 4 || dr == 5) && !(v->cr4 & (1ULL << 3))) dr += 2;
	if (dr == 4 || dr == 5) { inject_exception(v, 6, 0, 0); return RUN_GUEST; }   /* #UD */
	if (from) {
		uint64_t val = dr < 4 ? v->dr[dr] : dr == 6 ? v->dr6 : vmread(GUEST_DR7);
		gpr_set(v, reg, val);
	} else {
		uint64_t val = gpr_get(v, reg);
		if (dr < 4) v->dr[dr] = val;
		else if (dr == 6) v->dr6 = (val & 0xF00F) | 0xFFFF0FF0ULL;
		else vmwrite(GUEST_DR7, (val & 0xFFFFFFFF) | 0x400);
	}
	skip_instruction();
	return RUN_GUEST;
}

/* A guest access to guest-physical memory it may not reach: a slot's page not
 * mapped yet (map it, outside), or no slot at all, which is device memory. */
static int exit_ept_violation(kvm_vcpu_t *v)
{
	uint64_t gpa = vmread(GUEST_PHYSICAL_ADDRESS);
	if (v->apic && (v->apic_base & (1ULL << 11)) && (gpa & ~0xFFFULL) == (v->apic_base & ~0xFFFULL)) {
		switch (kvm_mmio_apic(v, gpa)) {
		case KVM_MMIO_RETRY: return RUN_OUTER;
		case KVM_MMIO_GUEST: return RUN_GUEST;
		default:             return RUN_USER;
		}
	}
	int present = kvm_slot_present(v->vm, gpa);
	if (present != 0) {
		/* a slot (or the slots changing): map the page in the outer loop,
		 * where a miss is decided under the lock */
		v->prefault = gpa & ~(uint64_t)(PAGE_SIZE - 1);
		return RUN_OUTER;
	}
	switch (kvm_mmio_exit(v, gpa)) {
	case KVM_MMIO_RETRY: return RUN_OUTER;
	case KVM_MMIO_GUEST: return RUN_GUEST;
	default:             return RUN_USER;
	}
}

static uint64_t tsc_khz_now(void)
{
	return tsc_mult ? (1000000ULL << 28) / tsc_mult : 0;
}

/* Stop the guest at its APIC timer's deadline: the preemption timer counts
 * down the TSC shifted by preempt_shift. Without one, the host's own timer
 * interrupts end each run often enough for kvm_lapic_timer_check. */
static void preempt_timer(kvm_vcpu_t *v)
{
	uint32_t pin = vmx.pin;
	if (vmx.preempt && v->apic && v->apic->timer_end) {
		uint64_t now = mono_ns(), end = v->apic->timer_end;
		uint64_t ns = end > now ? end - now : 0;
		uint64_t ticks = (ns * tsc_khz_now() / 1000000) >> vmx.preempt_shift;
		vmwrite(VMX_PREEMPTION_TIMER, ticks > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : ticks);
		pin |= PIN_PREEMPT_TIMER;
	}
	if (pin != v->pin_ctl) {
		v->pin_ctl = pin;
		vmwrite(PIN_BASED_CONTROLS, pin);
	}
}

static void set_cpu_ctl(kvm_vcpu_t *v, uint32_t ctl)
{
	if (ctl == v->cpu_ctl) return;
	v->cpu_ctl = ctl;
	vmwrite(CPU_BASED_CONTROLS, ctl);
}

/* Before an entry: deliver what KVM_NMI and KVM_INTERRUPT queued when the
 * guest can take it, and otherwise ask for an exit at the moment it can (the
 * NMI and interrupt windows). An event the CPU was delivering at the last exit
 * (requeue_vectoring) goes first. */
static void inject_events(kvm_vcpu_t *v)
{
	uint32_t ctl = v->cpu_ctl & ~(CPU_INTR_WINDOW_EXIT | CPU_NMI_WINDOW_EXIT);
	if (!(vmread(VM_ENTRY_INTR_INFO) & INTR_INFO_VALID)) {
		uint32_t is = (uint32_t)vmread(GUEST_INTERRUPTIBILITY);
		int shadow = is & (INTERRUPTIBILITY_STI | INTERRUPTIBILITY_MOVSS);
		if (v->nmi_pending && !shadow && !(is & INTERRUPTIBILITY_NMI)) {
			vmwrite(VM_ENTRY_INTR_INFO, INTR_INFO_VALID | INTR_TYPE_NMI | 2);
			v->nmi_pending = 0;
		} else if (!shadow && (vmread(GUEST_RFLAGS) & RFLAGS_IF)) {
			int vec;
			if (v->irq_pending >= 0) {   /* the VMM's PIC (ExtINT) */
				vmwrite(VM_ENTRY_INTR_INFO, INTR_INFO_VALID | INTR_TYPE_EXT | (uint32_t)v->irq_pending);
				v->irq_pending = -1;
			} else if ((vec = kvm_lapic_pending(v)) >= 0) {
				vmwrite(VM_ENTRY_INTR_INFO, INTR_INFO_VALID | INTR_TYPE_EXT | (uint32_t)vec);
				kvm_lapic_ack(v, vec);
			}
		}
	}
	if (v->nmi_pending) ctl |= CPU_NMI_WINDOW_EXIT;
	/* the VMM's window is for its PIC: only worth an exit while the APIC
	 * takes PIC interrupts, or the guest would exit at every entry */
	if (v->irq_pending >= 0 || kvm_lapic_pending(v) >= 0 ||
	    (v->run->request_interrupt_window && kvm_lapic_accepts_pic(v)))
		ctl |= CPU_INTR_WINDOW_EXIT;
	set_cpu_ctl(v, ctl);
	preempt_timer(v);
}

/* What userspace reads after every KVM_RUN to decide on KVM_INTERRUPT */
static void run_report(kvm_vcpu_t *v)
{
	struct kvm_run *run = v->run;
	uint64_t rflags = vmread(GUEST_RFLAGS);
	uint32_t is = (uint32_t)vmread(GUEST_INTERRUPTIBILITY);
	run->if_flag = (rflags & RFLAGS_IF) != 0;
	run->ready_for_interrupt_injection = run->if_flag &&
		!(is & (INTERRUPTIBILITY_STI | INTERRUPTIBILITY_MOVSS)) &&
		v->irq_pending < 0 && !(vmread(VM_ENTRY_INTR_INFO) & INTR_INFO_VALID) &&
		kvm_lapic_accepts_pic(v);
	if (v->apic) run->cr8 = kvm_lapic_cr8(v);
	run->apic_base = v->apic_base;
	run->flags = 0;
}

/* What ends a HLT: an NMI or INIT always, an interrupt when IF was set */
static int halt_wakes(kvm_vcpu_t *v)
{
	if (v->nmi_pending || v->init_pending) return 1;
	return v->halt_if && (v->irq_pending >= 0 || kvm_lapic_pending(v) >= 0);
}

static int handle_exit(kvm_vcpu_t *v, int cpu)
{
	uint32_t reason = (uint32_t)vmread(VM_EXIT_REASON);
	if (reason & EXIT_REASON_ENTRY_FAILED) return fail_entry(v, reason, cpu);
	requeue_vectoring();
	switch (reason & 0xFFFF) {
	case EXIT_EXTERNAL_INTR:
		/* it is still pending at the local APIC: let the host take it. The
		 * nop: an interrupt is recognised only after the instruction that
		 * follows STI. */
		__asm__ volatile("sti; nop; cli" ::: "memory");
		return host_wants_cpu() || v->init_pending ? RUN_OUTER : RUN_GUEST;
	case EXIT_EXCEPTION_NMI: {
		uint32_t info = (uint32_t)vmread(VM_EXIT_INTR_INFO);
		if ((info & INTR_TYPE_MASK) == INTR_TYPE_NMI) {
			__asm__ volatile("int $2");   /* the host's NMI, as if it came now */
			return RUN_GUEST;
		}
		return internal_error(v, KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON, reason, info);
	}
	case EXIT_TRIPLE_FAULT:
		v->run->exit_reason = KVM_EXIT_SHUTDOWN;
		return RUN_USER;
	case EXIT_IO:       return exit_io(v);
	case EXIT_CPUID:    return exit_cpuid(v);
	case EXIT_CR_ACCESS: return exit_cr(v);
	case EXIT_XSETBV:   return exit_xsetbv(v);
	case EXIT_HLT:
		skip_instruction();
		if (v->apic) {
			/* the APIC is here: the halt is too, until an interrupt */
			v->halt_if = (vmread(GUEST_RFLAGS) & RFLAGS_IF) != 0;
			if (halt_wakes(v)) return RUN_GUEST;
			v->mp_state = KVM_MP_STATE_HALTED;
			return RUN_OUTER;
		}
		v->run->exit_reason = KVM_EXIT_HLT;
		return RUN_USER;
	case EXIT_PREEMPT_TIMER:
		return RUN_GUEST;   /* the APIC timer fires at the next entry */
	case EXIT_INVD:     /* nothing cached the guest could lose: as WBINVD */
	case EXIT_MWAIT:    /* not offered in CPUID: a no-op, as Linux's KVM */
	case EXIT_MONITOR:
		skip_instruction();
		return RUN_GUEST;
	case EXIT_EPT_VIOLATION: return exit_ept_violation(v);
	case EXIT_RDMSR:    return exit_msr(v, 0);
	case EXIT_WRMSR:    return exit_msr(v, 1);
	case EXIT_DR_ACCESS: return exit_dr(v);
	case EXIT_VMCALL:   /* no hypercalls: #UD, as a CPU without VMX would */
		inject_exception(v, 6, 0, 0);
		return RUN_GUEST;
	case EXIT_INTR_WINDOW:
		if (v->irq_pending >= 0 || !v->run->request_interrupt_window || !kvm_lapic_accepts_pic(v))
			return RUN_GUEST;   /* ours to deliver (inject_events), not the VMM's */
		v->run->exit_reason = KVM_EXIT_IRQ_WINDOW_OPEN;
		return RUN_USER;
	case EXIT_NMI_WINDOW:
		return RUN_GUEST;
	default:
		return internal_error(v, KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON, reason, (uint64_t)cpu);
	}
}

static void fpu_load(kvm_vcpu_t *v)
{
	if (avx_on) __asm__ volatile("xrstor64 %0" :: "m"(*v->fpu), "a"(7), "d"(0) : "memory");
	else __asm__ volatile("fxrstor64 %0" :: "m"(*v->fpu) : "memory");
}

static void fpu_save(kvm_vcpu_t *v)
{
	if (avx_on) __asm__ volatile("xsave64 %0" : "=m"(*v->fpu) : "a"(7), "d"(0) : "memory");
	else __asm__ volatile("fxsave64 %0" : "=m"(*v->fpu) :: "memory");
}

/* The inner loop: VMCS loaded, interrupts off, guest FPU in the registers. */
static int run_loaded(kvm_vcpu_t *v)
{
	kvm_vm_t *vm = v->vm;
	uint64_t f;
	int cpu = vmcs_load(v, &f);
	if (cpu < 0) return cpu;
	host_state();
	kvm_msr_host_save(v);
	if (v->apic) apic_events(v);
	fpu_load(v);
	v->guest_cpu = cpu;
	smp_mb();   /* guest_cpu before ept_gen: see kvm_vm_quiesce */
	int launched = 0, ret;
	for (;;) {
		uint64_t gen = vm->ept_gen;
		if (vm->cpu_gen[cpu] != gen) {
			vmx_invept_single(vm->eptp);
			vm->cpu_gen[cpu] = gen;
		}
		v->seen_gen = gen;
		if (v->apic) kvm_lapic_timer_check(v, mono_ns());
		/* SYSCALL and SWAPGS reach the switched MSRs only with EFER.SCE
		 * or in long mode; a guest using neither need not pay for them */
		uint32_t nmsr = (v->efer & (EFER_SCE | EFER_LMA)) ? KVM_SWITCHED_MSRS : 0;
		if (nmsr != v->msr_count) {
			vmwrite(VM_ENTRY_MSR_LOAD_COUNT, nmsr);
			vmwrite(VM_EXIT_MSR_STORE_COUNT, nmsr);
			vmwrite(VM_EXIT_MSR_LOAD_COUNT, nmsr);
			v->msr_count = nmsr;
		}
		inject_events(v);
		if (v->xcr0 != 7 && avx_on) xsetbv(v->xcr0);
		int bad = vmx_enter(&v->g, launched);
		if (v->xcr0 != 7 && avx_on) xsetbv(7);
		if (bad) {
			ret = fail_entry(v, vmread(VM_INSTRUCTION_ERROR), cpu);
			break;
		}
		launched = 1;
		ret = handle_exit(v, cpu);
		if (ret != RUN_GUEST) break;
	}
	v->guest_cpu = -1;
	if (ret == RUN_USER) run_report(v);
	fpu_save(v);
	vmcs_put(v, f);
	return ret;
}

/* Sleep until kicked, a signal, or end (0: none) */
static void vcpu_block(kvm_vcpu_t *v, uint64_t end)
{
	__atomic_store_n(&v->blocked, 1, __ATOMIC_SEQ_CST);
	task_sleep_flag_until_ns(&v->kick, end);
	__atomic_store_n(&v->blocked, 0, __ATOMIC_SEQ_CST);
}

/* With the APIC here, a vcpu may have nothing to run: waiting for its SIPI,
 * or halted. 1: go into the guest; 0: it slept, look again. */
static int vcpu_ready(kvm_vcpu_t *v)
{
	__atomic_store_n(&v->kick, 0, __ATOMIC_SEQ_CST);   /* before the tests: a later kick wakes the sleep */
	if (v->init_pending) {
		v->init_pending = 0;
		v->do_reset = 1;
		v->mp_state = KVM_MP_STATE_INIT_RECEIVED;
	}
	if (v->mp_state == KVM_MP_STATE_UNINITIALIZED || v->mp_state == KVM_MP_STATE_INIT_RECEIVED) {
		if (!v->sipi_pending) { vcpu_block(v, 0); return 0; }
		v->sipi_pending = 0;
		v->do_sipi = 1;
		v->mp_state = KVM_MP_STATE_RUNNABLE;
	}
	if (v->mp_state == KVM_MP_STATE_HALTED) {
		kvm_lapic_timer_check(v, mono_ns());
		if (!halt_wakes(v)) { vcpu_block(v, v->apic->timer_end); return 0; }
		v->mp_state = KVM_MP_STATE_RUNNABLE;
	}
	return 1;
}

int vmx_vcpu_run(kvm_vcpu_t *v)
{
	struct kvm_run *run = v->run;
	v->thread = task_current();
	pio_finish(v);
	kvm_mmio_finish(v);
	int fr = kvm_pio_string_finish(v);
	if (fr) return fr;
	if (run->immediate_exit) {
		run->exit_reason = KVM_EXIT_INTR;
		return -EINTR;
	}
	for (;;) {
		if (kloop_should_stop() || task_has_actionable_signal()) {
			run->exit_reason = KVM_EXIT_INTR;
			return -EINTR;
		}
		if (v->apic && !vcpu_ready(v)) continue;
		if (v->prefault != NO_PREFAULT) {
			uint64_t gpa = v->prefault;
			v->prefault = NO_PREFAULT;
			int r = kvm_ept_fault(v->vm, gpa);
			if (r < 0) return r;
			/* r == 0: the slot went away meanwhile; the guest's access
			 * exits again and is decided as device memory then */
		}
		int r = run_loaded(v);
		if (r < 0) return r;
		if (r == RUN_USER) return 0;
	}
}
