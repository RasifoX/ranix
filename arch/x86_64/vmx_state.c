/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A vcpu's state as userspace gets and sets it: registers, segments and
 * control registers, MSRs, pending events, debug registers, x87/SSE/AVX state
 * and XCR0. Each ioctl loads the VMCS for its fields (vmcs_load) and clears it
 * again, as KVM_RUN does, so it may come from any CPU.
 *
 * Provenance: structure meanings from Linux's public KVM API description
 * (api.rst); the FXSAVE/XSAVE layouts and the VMCS guest-state fields from the
 * Intel SDM (Vol. 1 chapter 13, Vol. 3 chapter 24). Original code. */

#include <asm/kvm.h>
#include <asm/vmx.h>
#include <asm/cpu.h>
#include <uapi/kvm.h>
#include <kernel/hal.h>
#include <kernel/errno.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/pmm.h>
#include <kernel/lib/string.h>

#define CR0_PG  (1ULL << 31)
#define CR4_PAE (1ULL << 5)

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
	for (int i = 0; i < 8; i++) vmx_seg_get(sregs_seg[i], &segs[i]);
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
	if (v->irq_pending >= 0)   /* the interrupt KVM_INTERRUPT queued, not yet taken */
		s->interrupt_bitmap[v->irq_pending / 64] |= 1ULL << (v->irq_pending % 64);
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
	for (int i = 0; i < 8; i++) vmx_seg_set(sregs_seg[i], &segs[i]);
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
	v->irq_pending = -1;
	for (int i = 0; i < KVM_NR_INTERRUPTS; i++)
		if (s->interrupt_bitmap[i / 64] & (1ULL << (i % 64))) { v->irq_pending = i; break; }
	vmx_cr0_apply(v);
	vmx_cr4_apply(v);
	vmx_efer_update(v);
	int r = vmx_pdptrs_load(v);
	vmcs_put(v, f);
	return r == -EAGAIN ? -EFAULT : r;
}

/* KVM_GET_MSRS / KVM_SET_MSRS: as many entries as go through, in order; the
 * count is the result, as on Linux */
int vmx_get_msrs(kvm_vcpu_t *v, struct kvm_msrs *u, int set)
{
	struct kvm_msrs h;
	if (copy_from_user(&h, u, sizeof(h))) return -EFAULT;
	if (h.nmsrs > 1024) return -E2BIG;
	size_t bytes = h.nmsrs * sizeof(struct kvm_msr_entry);
	struct kvm_msr_entry *e = h.nmsrs ? kmalloc(bytes) : NULL;
	if (h.nmsrs && !e) return -ENOMEM;
	if (bytes && copy_from_user(e, u->entries, bytes)) { kfree(e); return -EFAULT; }
	uint64_t f;
	int cpu = vmcs_load(v, &f);
	if (cpu < 0) { kfree(e); return cpu; }
	uint32_t n = 0;
	for (; n < h.nmsrs; n++)
		if (set ? kvm_msr_set(v, e[n].index, e[n].data) : kvm_msr_get(v, e[n].index, &e[n].data)) break;
	vmcs_put(v, f);
	int r = (int)n;
	if (!set && n && copy_to_user(u->entries, e, n * sizeof(*e))) r = -EFAULT;
	kfree(e);
	return r;
}

int vmx_get_events(kvm_vcpu_t *v, struct kvm_vcpu_events *e)
{
	uint64_t f;
	memset(e, 0, sizeof(*e));
	int cpu = vmcs_load(v, &f);
	if (cpu < 0) return cpu;
	uint32_t info = (uint32_t)vmread(VM_ENTRY_INTR_INFO);
	uint32_t is = (uint32_t)vmread(GUEST_INTERRUPTIBILITY);
	vmcs_put(v, f);
	uint32_t type = info & INTR_TYPE_MASK;
	if ((info & INTR_INFO_VALID) && (type == INTR_TYPE_HW_EXC || type == (6U << 8))) {
		e->exception.injected = 1;
		e->exception.nr = (uint8_t)info;
		e->exception.has_error_code = (info & INTR_INFO_ERRCODE) != 0;
	} else if ((info & INTR_INFO_VALID) && (type == INTR_TYPE_EXT || type == INTR_TYPE_SW_INT)) {
		e->interrupt.injected = 1;
		e->interrupt.nr = (uint8_t)info;
		e->interrupt.soft = type == INTR_TYPE_SW_INT;
	} else if (v->irq_pending >= 0) {
		e->interrupt.injected = 1;
		e->interrupt.nr = (uint8_t)v->irq_pending;
	}
	e->interrupt.shadow = ((is & INTERRUPTIBILITY_STI) ? KVM_X86_SHADOW_INT_STI : 0) |
	                      ((is & INTERRUPTIBILITY_MOVSS) ? KVM_X86_SHADOW_INT_MOV_SS : 0);
	e->nmi.injected = (info & INTR_INFO_VALID) && type == INTR_TYPE_NMI;
	e->nmi.pending = v->nmi_pending;
	e->nmi.masked = (is & INTERRUPTIBILITY_NMI) != 0;
	e->flags = KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SHADOW;
	return 0;
}

int vmx_set_events(kvm_vcpu_t *v, const struct kvm_vcpu_events *e)
{
	uint64_t f;
	int cpu = vmcs_load(v, &f);
	if (cpu < 0) return cpu;
	uint32_t info = 0;
	if (e->exception.injected) {
		info = INTR_INFO_VALID | INTR_TYPE_HW_EXC | e->exception.nr;
		if (e->exception.has_error_code && (v->cr0 & 1)) {
			info |= INTR_INFO_ERRCODE;
			vmwrite(VM_ENTRY_EXCEPTION_ERROR_CODE, e->exception.error_code);
		}
	} else if (e->nmi.injected) {
		info = INTR_INFO_VALID | INTR_TYPE_NMI | 2;
	}
	vmwrite(VM_ENTRY_INTR_INFO, info);
	v->irq_pending = (e->interrupt.injected && !e->interrupt.soft) ? e->interrupt.nr : -1;
	uint32_t is = (uint32_t)vmread(GUEST_INTERRUPTIBILITY) & ~(INTERRUPTIBILITY_STI | INTERRUPTIBILITY_MOVSS);
	if (e->flags & KVM_VCPUEVENT_VALID_SHADOW) {
		if (e->interrupt.shadow & KVM_X86_SHADOW_INT_STI) is |= INTERRUPTIBILITY_STI;
		if (e->interrupt.shadow & KVM_X86_SHADOW_INT_MOV_SS) is |= INTERRUPTIBILITY_MOVSS;
	}
	if (e->nmi.masked) is |= INTERRUPTIBILITY_NMI;
	else is &= ~INTERRUPTIBILITY_NMI;
	vmwrite(GUEST_INTERRUPTIBILITY, is);
	if (e->flags & KVM_VCPUEVENT_VALID_NMI_PENDING) v->nmi_pending = e->nmi.pending != 0;
	vmcs_put(v, f);
	return 0;
}

int vmx_get_debugregs(kvm_vcpu_t *v, struct kvm_debugregs *d)
{
	uint64_t f;
	memset(d, 0, sizeof(*d));
	int cpu = vmcs_load(v, &f);
	if (cpu < 0) return cpu;
	d->dr7 = vmread(GUEST_DR7);
	vmcs_put(v, f);
	memcpy(d->db, v->dr, sizeof(d->db));
	d->dr6 = v->dr6;
	return 0;
}

int vmx_set_debugregs(kvm_vcpu_t *v, const struct kvm_debugregs *d)
{
	if (d->flags || (d->dr6 >> 32) || (d->dr7 >> 32)) return -EINVAL;
	uint64_t f;
	int cpu = vmcs_load(v, &f);
	if (cpu < 0) return cpu;
	vmwrite(GUEST_DR7, d->dr7 | 0x400);
	vmcs_put(v, f);
	memcpy(v->dr, d->db, sizeof(v->dr));
	v->dr6 = d->dr6;
	return 0;
}

/* XCR0 from XSETBV or KVM_SET_XCRS: only states the host has on (x87, SSE,
 * AVX), x87 always, AVX only with SSE. -1 for a #GP. */
int vmx_set_xcr0(kvm_vcpu_t *v, uint64_t val)
{
	if (!avx_on || !(val & 1) || (val & ~7ULL) || ((val & 4) && !(val & 2))) return -1;
	v->xcr0 = val;
	return 0;
}

/* KVM_SET_XSAVE: XRSTOR would fault in the kernel on a bad header or MXCSR,
 * so those are checked here: only x87, SSE and AVX state, standard format,
 * reserved header bytes and MXCSR bits zero. */
int vmx_set_xsave(kvm_vcpu_t *v, const uint8_t *area)
{
	uint64_t xstate_bv, xcomp_bv;
	uint32_t mxcsr;
	memcpy(&xstate_bv, area + 512, 8);
	memcpy(&xcomp_bv, area + 520, 8);
	memcpy(&mxcsr, area + 24, 4);
	if ((xstate_bv & ~7ULL) || xcomp_bv || (mxcsr & 0xFFFF0000U)) return -EINVAL;
	for (int i = 528; i < 576; i++) if (area[i]) return -EINVAL;
	memcpy(v->fpu, area, PAGE_SIZE);
	return 0;
}
