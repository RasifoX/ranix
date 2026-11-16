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
#include <kernel/errno.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/lib/string.h>

#define CR0_PE (1ULL << 0)
#define CR0_TS (1ULL << 3)
#define CR0_NW (1ULL << 29)
#define CR0_CD (1ULL << 30)
#define CR0_PG (1ULL << 31)
#define CR4_PAE     (1ULL << 5)
#define CR4_OSXSAVE (1ULL << 18)
#define RFLAGS_IF   (1ULL << 9)

/* inner-loop results */
#define RUN_GUEST 1   /* enter the guest again */
#define RUN_OUTER 2   /* leave the inner loop, do the slow work, come back */
#define RUN_USER  0   /* return to userspace, kvm_run says why */

#define NO_PREFAULT (~0ULL)

static struct {
	uint32_t rev;                     /* VMCS revision identifier */
	uint32_t pin, cpu, cpu2, exit, entry;
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
	               PIN_EXT_INTR_EXIT | PIN_NMI_EXIT, 0, &vmx.pin) ||
	    ctl_adjust(tru ? MSR_IA32_VMX_TRUE_PROCBASED : MSR_IA32_VMX_PROCBASED,
	               CPU_HLT_EXIT | CPU_UNCOND_IO_EXIT | CPU_MWAIT_EXIT | CPU_MONITOR_EXIT |
	               CPU_CR8_LOAD_EXIT | CPU_CR8_STORE_EXIT | CPU_SECONDARY, 0, &vmx.cpu) ||
	    ctl_adjust(MSR_IA32_VMX_PROCBASED2, CPU2_EPT | CPU2_UNRESTRICTED_GUEST, 0, &vmx.cpu2) ||
	    ctl_adjust(tru ? MSR_IA32_VMX_TRUE_EXIT : MSR_IA32_VMX_EXIT,
	               EXIT_HOST_ADDR64 | EXIT_SAVE_EFER | EXIT_LOAD_EFER | EXIT_SAVE_PAT | EXIT_LOAD_PAT,
	               0, &vmx.exit) ||
	    ctl_adjust(tru ? MSR_IA32_VMX_TRUE_ENTRY : MSR_IA32_VMX_ENTRY,
	               ENTRY_LOAD_EFER | ENTRY_LOAD_PAT, 0, &vmx.entry)) {
		kprintf(KERN_INFO "kvm: VT-x lacks a control this KVM needs (EPT, unrestricted guest, EFER/PAT switch)\n");
		return -ENODEV;
	}
	/* No TPR shadow: a guest's CR8 is the VMM's, through the CR8 exits. A CPU
	 * that forced it on would want a virtual-APIC page. */
	if (vmx.cpu & CPU_TPR_SHADOW) return -ENODEV;
	vmx.entry &= ~ENTRY_IA32E_GUEST;   /* set per vcpu from its EFER.LMA */

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
static int vmcs_load(kvm_vcpu_t *v, uint64_t *irqf)
{
	*irqf = irq_save();
	int cpu = this_cpu();
	int r = vmx_cpu_on(cpu);
	if (!r && vmx_ptrld(v->vmcs)) r = -EIO;
	if (r) { irq_restore(*irqf); return r; }
	return cpu;
}

static void vmcs_put(kvm_vcpu_t *v, uint64_t irqf)
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

static void seg_set(int i, const struct kvm_segment *s)
{
	uint32_t ar;
	if (s->unusable || !s->present)
		ar = AR_UNUSABLE;
	else
		ar = (s->type & 15) | (s->s & 1) << 4 | (s->dpl & 3) << 5 | 1U << 7 |
		     (s->avl & 1) << 12 | (s->l & 1) << 13 | (s->db & 1) << 14 | (s->g & 1) << 15;
	seg_write(i, s->selector, s->base, s->limit, ar);
}

static void seg_get(int i, struct kvm_segment *s)
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
static void efer_apply(kvm_vcpu_t *v)
{
	vmwrite(GUEST_IA32_EFER, v->efer);
	uint32_t entry = vmx.entry | ((v->efer & EFER_LMA) ? ENTRY_IA32E_GUEST : 0);
	vmwrite(VM_ENTRY_CONTROLS, entry);
}

/* The guest's CR0 is what it wrote (the read shadow shows it back); the CPU
 * runs with the bits VMX operation fixes, and with caching on: CD and NW stay
 * the VMM's, as on Linux's KVM. PE and PG are free under unrestricted guest. */
static void cr0_apply(kvm_vcpu_t *v)
{
	uint64_t hw = (v->cr0 | (vmx.cr0_fixed0 & ~(CR0_PE | CR0_PG))) & vmx.cr0_fixed1;
	hw &= ~(CR0_CD | CR0_NW);
	vmwrite(GUEST_CR0, hw);
	vmwrite(CR0_READ_SHADOW, v->cr0);
}

static void cr4_apply(kvm_vcpu_t *v)
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
	cr0_apply(v);
	efer_apply(v);
}

/* PAE paging outside long mode takes its four PDPTEs from the VMCS, not from
 * memory: load them when CR0, CR3 or CR4 changes. -EAGAIN when the guest page
 * holding them is not mapped yet (v->prefault names it). */
static int pdptrs_load(kvm_vcpu_t *v)
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

/* Controls and the parts of the host state that never change, then the
 * architectural reset state (Intel SDM 9.1.1, as Linux's KVM presents it:
 * CS f000:fff0 with base ffff0000, real mode, caching off). */
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
	vmwrite(VM_EXIT_MSR_STORE_COUNT, 0);
	vmwrite(VM_EXIT_MSR_LOAD_COUNT, 0);
	vmwrite(VM_ENTRY_MSR_LOAD_COUNT, 0);
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
	cr0_apply(v);
	cr4_apply(v);
	efer_apply(v);
}

int vmx_vcpu_init(kvm_vcpu_t *v)
{
	uint64_t vmcs = (uint64_t)buddy_alloc(0);
	uint64_t fpu = (uint64_t)buddy_alloc(0);
	if (!vmcs || !fpu) goto nomem;
	memset((void *)PHYS_TO_VIRT(vmcs), 0, PAGE_SIZE);
	*(uint32_t *)PHYS_TO_VIRT(vmcs) = vmx.rev;
	v->vmcs = vmcs;
	v->fpu = (uint8_t *)PHYS_TO_VIRT(fpu);
	memset(v->fpu, 0, PAGE_SIZE);
	*(uint16_t *)&v->fpu[0]  = 0x037F;   /* FCW, as after FNINIT */
	*(uint32_t *)&v->fpu[24] = 0x1F80;   /* MXCSR */
	v->xcr0 = 1;
	v->apic_base = 0xFEE00000ULL | (1ULL << 11) | (v->id == 0 ? 1ULL << 8 : 0);   /* enabled, BSP */
	v->g.r[R_DX] = 0x600;   /* the reset value of EDX: family 6 */
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
	v->vmcs = 0;
	v->fpu = NULL;
	return r;
}

void vmx_vcpu_free(kvm_vcpu_t *v)
{
	/* never current anywhere: every load ends in VMCLEAR */
	if (v->vmcs) buddy_free(v->vmcs, 0);
	if (v->fpu) buddy_free(VIRT_TO_PHYS(v->fpu), 0);
	v->vmcs = 0;
	v->fpu = NULL;
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
	if (string || (size != 1 && size != 2 && size != 4))
		return internal_error(v, KVM_INTERNAL_ERROR_EMULATION, EXIT_IO, q);
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

/* CPUID answers the host's, minus what this KVM does not give a guest: VMX
 * (no nested guests), and OSXSAVE following the guest's own CR4. */
static int exit_cpuid(kvm_vcpu_t *v)
{
	uint32_t r[4];
	cpuid((uint32_t)v->g.r[R_AX], (uint32_t)v->g.r[R_CX], r);
	if ((uint32_t)v->g.r[R_AX] == 1) {
		r[2] &= ~((1U << 5) | (1U << 27));
		if (v->cr4 & CR4_OSXSAVE) r[2] |= 1U << 27;
		r[2] |= 1U << 31;   /* running under a hypervisor */
	}
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
		case 4: v->cr4 = val; cr4_apply(v); break;
		case 8: v->run->cr8 = val & 15; break;
		default: return internal_error(v, KVM_INTERNAL_ERROR_EMULATION, EXIT_CR_ACCESS, q);
		}
		if (cr != 8) {
			int r = pdptrs_load(v);
			if (r == -EAGAIN) return RUN_OUTER;   /* map the PDPT, run the write again */
			if (r) { inject_exception(v, 13, 1, 0); return RUN_GUEST; }
		}
	} else if (type == 1) {
		if (cr == 3) gpr_set(v, reg, vmread(GUEST_CR3));
		else if (cr == 8) gpr_set(v, reg, v->run->cr8);
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

/* XSETBV: XCR0 is loaded for the guest around each entry. Only states the
 * host has on (x87, SSE, AVX) are allowed, in the combinations the SDM does. */
static int exit_xsetbv(kvm_vcpu_t *v)
{
	uint64_t val = (v->g.r[R_DX] << 32) | (uint32_t)v->g.r[R_AX];
	if ((uint32_t)v->g.r[R_CX] != 0 || !avx_on || !(val & 1) || (val & ~7ULL) ||
	    ((val & 4) && !(val & 2))) {
		inject_exception(v, 13, 1, 0);
		return RUN_GUEST;
	}
	v->xcr0 = val;
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
		return host_wants_cpu() ? RUN_OUTER : RUN_GUEST;
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
		v->run->exit_reason = KVM_EXIT_HLT;
		return RUN_USER;
	case EXIT_INVD:     /* nothing cached the guest could lose: as WBINVD */
	case EXIT_MWAIT:    /* not offered in CPUID: a no-op, as Linux's KVM */
	case EXIT_MONITOR:
		skip_instruction();
		return RUN_GUEST;
	case EXIT_EPT_VIOLATION:
		v->prefault = vmread(GUEST_PHYSICAL_ADDRESS) & ~(uint64_t)(PAGE_SIZE - 1);
		return RUN_OUTER;
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
	fpu_load(v);
	v->guest_cpu = cpu;
	smp_mb();   /* guest_cpu before ept_gen: see kvm_ept_quiesce */
	int launched = 0, ret;
	for (;;) {
		uint64_t gen = vm->ept_gen;
		if (vm->cpu_gen[cpu] != gen) {
			vmx_invept_single(vm->eptp);
			vm->cpu_gen[cpu] = gen;
		}
		v->seen_gen = gen;
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
	fpu_save(v);
	vmcs_put(v, f);
	return ret;
}

int vmx_vcpu_run(kvm_vcpu_t *v)
{
	struct kvm_run *run = v->run;
	pio_finish(v);
	if (run->immediate_exit) {
		run->exit_reason = KVM_EXIT_INTR;
		return -EINTR;
	}
	for (;;) {
		if (kloop_should_stop() || task_has_actionable_signal()) {
			run->exit_reason = KVM_EXIT_INTR;
			return -EINTR;
		}
		if (v->prefault != NO_PREFAULT) {
			uint64_t gpa = v->prefault;
			v->prefault = NO_PREFAULT;
			int r = kvm_ept_fault(v->vm, gpa);
			if (r < 0) return r;
			if (r == 0) {
				/* no memory slot there: device memory, which needs an
				 * instruction emulator this KVM does not have yet */
				internal_error(v, KVM_INTERNAL_ERROR_EMULATION, EXIT_EPT_VIOLATION, gpa);
				return 0;
			}
		}
		int r = run_loaded(v);
		if (r < 0) return r;
		if (r == RUN_USER) return 0;
	}
}

/* ── register ioctls ──────────────────────────────────────── */

int vmx_get_regs(kvm_vcpu_t *v, struct kvm_regs *r)
{
	uint64_t f;
	int cpu = vmcs_load(v, &f);
	if (cpu < 0) return cpu;
	r->rax = v->g.r[R_AX]; r->rbx = v->g.r[R_BX]; r->rcx = v->g.r[R_CX]; r->rdx = v->g.r[R_DX];
	r->rsi = v->g.r[R_SI]; r->rdi = v->g.r[R_DI]; r->rbp = v->g.r[R_BP];
	r->r8  = v->g.r[8];  r->r9  = v->g.r[9];  r->r10 = v->g.r[10]; r->r11 = v->g.r[11];
	r->r12 = v->g.r[12]; r->r13 = v->g.r[13]; r->r14 = v->g.r[14]; r->r15 = v->g.r[15];
	r->rsp = vmread(GUEST_RSP);
	r->rip = vmread(GUEST_RIP);
	r->rflags = vmread(GUEST_RFLAGS);
	vmcs_put(v, f);
	return 0;
}

int vmx_set_regs(kvm_vcpu_t *v, const struct kvm_regs *r)
{
	uint64_t f;
	int cpu = vmcs_load(v, &f);
	if (cpu < 0) return cpu;
	v->g.r[R_AX] = r->rax; v->g.r[R_BX] = r->rbx; v->g.r[R_CX] = r->rcx; v->g.r[R_DX] = r->rdx;
	v->g.r[R_SI] = r->rsi; v->g.r[R_DI] = r->rdi; v->g.r[R_BP] = r->rbp;
	v->g.r[8]  = r->r8;  v->g.r[9]  = r->r9;  v->g.r[10] = r->r10; v->g.r[11] = r->r11;
	v->g.r[12] = r->r12; v->g.r[13] = r->r13; v->g.r[14] = r->r14; v->g.r[15] = r->r15;
	vmwrite(GUEST_RSP, r->rsp);
	vmwrite(GUEST_RIP, r->rip);
	vmwrite(GUEST_RFLAGS, (r->rflags & ~0x8028ULL) | 0x2);   /* reserved bits as the CPU has them */
	vmcs_put(v, f);
	v->pio_in = 0;   /* userspace replaced the registers an IN would finish into */
	return 0;
}

/* kvm_sregs keeps the segments in this order */
static const int sregs_seg[8] = { SEG_CS, SEG_DS, SEG_ES, SEG_FS, SEG_GS, SEG_SS, SEG_TR, SEG_LDTR };

int vmx_get_sregs(kvm_vcpu_t *v, struct kvm_sregs *s)
{
	uint64_t f;
	memset(s, 0, sizeof(*s));
	int cpu = vmcs_load(v, &f);
	if (cpu < 0) return cpu;
	struct kvm_segment *segs = &s->cs;
	for (int i = 0; i < 8; i++) seg_get(sregs_seg[i], &segs[i]);
	s->gdt.base = vmread(GUEST_GDTR_BASE);
	s->gdt.limit = (uint16_t)vmread(GUEST_GDTR_LIMIT);
	s->idt.base = vmread(GUEST_IDTR_BASE);
	s->idt.limit = (uint16_t)vmread(GUEST_IDTR_LIMIT);
	s->cr3 = vmread(GUEST_CR3);
	vmcs_put(v, f);
	s->cr0 = v->cr0;
	s->cr2 = v->g.cr2;
	s->cr4 = v->cr4;
	s->cr8 = v->run->cr8;
	s->efer = v->efer;
	s->apic_base = v->apic_base;
	return 0;
}

int vmx_set_sregs(kvm_vcpu_t *v, const struct kvm_sregs *s)
{
	/* PAE paging reads its PDPTEs from guest memory: map that page now, while
	 * this thread may still sleep */
	if ((s->cr0 & CR0_PG) && (s->cr4 & CR4_PAE) && !(s->efer & EFER_LMA)) {
		int r = kvm_ept_fault(v->vm, s->cr3 & ~0xFFFULL);
		if (r < 0) return r;
	}
	uint64_t f;
	int cpu = vmcs_load(v, &f);
	if (cpu < 0) return cpu;
	const struct kvm_segment *segs = &s->cs;
	for (int i = 0; i < 8; i++) seg_set(sregs_seg[i], &segs[i]);
	vmwrite(GUEST_GDTR_BASE, s->gdt.base);
	vmwrite(GUEST_GDTR_LIMIT, s->gdt.limit);
	vmwrite(GUEST_IDTR_BASE, s->idt.base);
	vmwrite(GUEST_IDTR_LIMIT, s->idt.limit);
	vmwrite(GUEST_CR3, s->cr3);
	v->g.cr2 = s->cr2;
	v->cr0 = s->cr0;
	v->cr4 = s->cr4;
	v->efer = s->efer;   /* as given, LMA too: userspace restores a state, it does not switch modes */
	v->run->cr8 = s->cr8 & 15;
	v->apic_base = s->apic_base;
	cr0_apply(v);
	cr4_apply(v);
	efer_apply(v);
	int r = pdptrs_load(v);
	vmcs_put(v, f);
	return r == -EAGAIN ? -EFAULT : r;
}
