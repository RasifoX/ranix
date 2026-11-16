/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* MSRs of a KVM guest: the guest's RDMSR/WRMSR (every one exits: there is no
 * MSR bitmap) and userspace's KVM_GET_MSRS/KVM_SET_MSRS, with the VMCS of the
 * vcpu loaded.
 *
 * Where each lives. The ones VMX switches itself (SYSENTER_*, PAT, DEBUGCTL,
 * EFER, FS/GS base) are VMCS guest fields. The SYSCALL MSRs and
 * KERNEL_GS_BASE are the host's real registers too: SWAPGS and SYSCALL in a
 * guest use them without an exit. They live in the vcpu's VM-entry MSR-load
 * area, which VM exit stores back into (so a guest SWAPGS is kept), and the
 * host's values come back from the VM-exit MSR-load area. The TSC is the host's
 * plus the VMCS TSC offset. MTRRs and MISC_ENABLE are only stored: guest
 * memory is write-back through EPT whatever they say, as on Linux's KVM.
 * Anything else is a #GP for the guest and an error for userspace.
 *
 * Provenance: MSR numbers and meanings from the Intel SDM Vol. 4 (MSRs) and
 * Vol. 3 (24.7.2, 24.8.2: the VM-exit and VM-entry MSR areas); the
 * get/set/index-list semantics from Linux's public KVM API description
 * (api.rst). Original code. */

#include <asm/kvm.h>
#include <asm/vmx.h>
#include <uapi/kvm.h>
#include <kernel/hal.h>
#include <kernel/errno.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>

#define MSR_TSC              0x10
#define MSR_PLATFORM_ID      0x17
#define MSR_APIC_BASE        0x1B
#define MSR_TSC_ADJUST       0x3B
#define MSR_BIOS_SIGN_ID     0x8B
#define MSR_MTRRCAP          0xFE
#define MSR_MCG_CAP          0x179
#define MSR_MCG_STATUS       0x17A
#define MSR_MCG_CTL          0x17B
#define MSR_MC0_CTL          0x400   /* CTL STATUS ADDR MISC for each bank */
#define MSR_MISC_ENABLE      0x1A0
#define MSR_DEBUGCTL         0x1D9
#define MSR_MTRR_PHYS0       0x200   /* base/mask pairs to 0x20F */
#define MSR_MTRR_FIX64K      0x250
#define MSR_MTRR_FIX16K_0    0x258
#define MSR_MTRR_FIX16K_1    0x259
#define MSR_MTRR_FIX4K_0     0x268   /* to 0x26F */
#define MSR_MTRR_DEF_TYPE    0x2FF
#define MSR_STAR             0xC0000081
#define MSR_LSTAR            0xC0000082
#define MSR_CSTAR            0xC0000083
#define MSR_SFMASK           0xC0000084
#define MSR_KERNEL_GS_BASE   0xC0000102
/* kvmclock, old and new numbers: not offered in CPUID, but a VMM resets them */
#define MSR_KVM_WALL_CLOCK      0x11
#define MSR_KVM_SYSTEM_TIME     0x12
#define MSR_KVM_WALL_CLOCK_NEW  0x4b564d00
#define MSR_KVM_SYSTEM_TIME_NEW 0x4b564d01

#define MTRR_VAR 8
#define MTRRCAP_VALUE (0x400 | 0x100 | MTRR_VAR)   /* WC, fixed ranges, 8 variable */

/* the switched MSRs, in their area order (KVM_SWITCHED_MSRS of them) */
static const uint32_t switched[KVM_SWITCHED_MSRS] = {
	MSR_STAR, MSR_LSTAR, MSR_CSTAR, MSR_SFMASK, MSR_KERNEL_GS_BASE,
};

/* what userspace should save and restore (KVM_GET_MSR_INDEX_LIST) */
static const uint32_t saved[] = {
	MSR_IA32_SYSENTER_CS, MSR_IA32_SYSENTER_ESP, MSR_IA32_SYSENTER_EIP,
	MSR_STAR, MSR_LSTAR, MSR_CSTAR, MSR_SFMASK, MSR_KERNEL_GS_BASE,
	MSR_TSC, MSR_IA32_PAT, MSR_MISC_ENABLE,
};

static uint64_t rdtsc(void)
{
	uint32_t lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
}

static int switched_slot(uint32_t idx)
{
	for (int i = 0; i < KVM_SWITCHED_MSRS; i++)
		if (switched[i] == idx) return i;
	return -1;
}

/* the machine-check MSRs a VMM set up (KVM_X86_SETUP_MCE): their slot, or -1 */
static uint64_t *mce_reg(kvm_vcpu_t *v, uint32_t idx)
{
	uint32_t banks = (uint32_t)(v->mcg_cap & 0xFF);
	if (idx == MSR_MCG_CTL) return (v->mcg_cap & (1ULL << 8)) ? &v->mcg_ctl : NULL;
	if (idx >= MSR_MC0_CTL && idx < MSR_MC0_CTL + 4 * banks) return &v->mce_bank[idx - MSR_MC0_CTL];
	return NULL;
}

/* the 11 fixed-range MTRRs as one array */
static int fixed_slot(uint32_t idx)
{
	if (idx == MSR_MTRR_FIX64K) return 0;
	if (idx == MSR_MTRR_FIX16K_0) return 1;
	if (idx == MSR_MTRR_FIX16K_1) return 2;
	if (idx >= MSR_MTRR_FIX4K_0 && idx <= MSR_MTRR_FIX4K_0 + 7) return 3 + (int)(idx - MSR_MTRR_FIX4K_0);
	return -1;
}

static int canonical(uint64_t a) { return (uint64_t)((int64_t)(a << 16) >> 16) == a; }

void kvm_msr_reset(kvm_vcpu_t *v)
{
	for (int i = 0; i < KVM_SWITCHED_MSRS; i++) {
		v->msr_guest[i].index = switched[i];
		v->msr_guest[i].data = 0;
		v->msr_host[i].index = switched[i];
	}
	v->misc_enable = 1;   /* fast strings */
	v->mtrr_def = 0;
	memset(v->mtrr_var, 0, sizeof(v->mtrr_var));
	memset(v->mtrr_fix, 0, sizeof(v->mtrr_fix));
}

/* The host's values of the switched MSRs, for the VM-exit load area. The
 * SYSCALL ones are per CPU; KERNEL_GS_BASE holds the user's GS base while the
 * kernel runs, so it is read for each run. */
void kvm_msr_host_save(kvm_vcpu_t *v)
{
	for (int i = 0; i < KVM_SWITCHED_MSRS; i++) v->msr_host[i].data = vmx_rdmsr(switched[i]);
}

/* An MSR this KVM does not have: said in the log, the first few times, so
 * what a guest or VMM wants next has a name. */
static int msr_unknown(uint32_t idx, int write)
{
	static int told;
	if (told < 16) {
		told++;
		kprintf(KERN_INFO "kvm: %s of MSR 0x%x not supported\n", write ? "write" : "read", idx);
	}
	return -1;
}

/* 0 with *val, or -1: the MSR does not exist for this guest */
int kvm_msr_get(kvm_vcpu_t *v, uint32_t idx, uint64_t *val)
{
	int s;
	switch (idx) {
	case MSR_IA32_SYSENTER_CS:  *val = vmread(GUEST_SYSENTER_CS); return 0;
	case MSR_IA32_SYSENTER_ESP: *val = vmread(GUEST_SYSENTER_ESP); return 0;
	case MSR_IA32_SYSENTER_EIP: *val = vmread(GUEST_SYSENTER_EIP); return 0;
	case MSR_IA32_PAT:          *val = vmread(GUEST_IA32_PAT); return 0;
	case MSR_DEBUGCTL:          *val = vmread(GUEST_IA32_DEBUGCTL); return 0;
	case MSR_EFER:              *val = v->efer; return 0;
	case MSR_FS_BASE_:          *val = vmread(GUEST_ES_BASE + 2 * SEG_FS); return 0;
	case MSR_GS_BASE_:          *val = vmread(GUEST_ES_BASE + 2 * SEG_GS); return 0;
	case MSR_TSC:               *val = rdtsc() + vmread(TSC_OFFSET); return 0;
	case MSR_APIC_BASE:         *val = v->apic_base; return 0;
	case MSR_IA32_FEATURE_CONTROL: *val = FEATURE_CONTROL_LOCK; return 0;   /* locked, VMX off */
	case MSR_PLATFORM_ID:
	case MSR_BIOS_SIGN_ID:
	case MSR_MCG_CAP:           *val = v->mcg_cap; return 0;
	case MSR_MCG_STATUS:        *val = v->mcg_status; return 0;
	case MSR_MISC_ENABLE:       *val = v->misc_enable; return 0;
	case MSR_KVM_WALL_CLOCK:
	case MSR_KVM_SYSTEM_TIME:
	case MSR_KVM_WALL_CLOCK_NEW:
	case MSR_KVM_SYSTEM_TIME_NEW: *val = 0; return 0;
	case MSR_MTRRCAP:           *val = MTRRCAP_VALUE; return 0;
	case MSR_MTRR_DEF_TYPE:     *val = v->mtrr_def; return 0;
	}
	if (idx >= MSR_MTRR_PHYS0 && idx < MSR_MTRR_PHYS0 + 2 * MTRR_VAR) {
		*val = v->mtrr_var[idx - MSR_MTRR_PHYS0];
		return 0;
	}
	if ((s = fixed_slot(idx)) >= 0) { *val = v->mtrr_fix[s]; return 0; }
	uint64_t *mce = mce_reg(v, idx);
	if (mce) { *val = *mce; return 0; }
	if ((s = switched_slot(idx)) >= 0) { *val = v->msr_guest[s].data; return 0; }
	return msr_unknown(idx, 0);
}

/* 0, or -1 for a #GP: the MSR does not exist or the value is not allowed */
int kvm_msr_set(kvm_vcpu_t *v, uint32_t idx, uint64_t val)
{
	int s;
	switch (idx) {
	case MSR_IA32_SYSENTER_CS:  vmwrite(GUEST_SYSENTER_CS, val & 0xFFFF); return 0;
	case MSR_IA32_SYSENTER_ESP: if (!canonical(val)) return -1; vmwrite(GUEST_SYSENTER_ESP, val); return 0;
	case MSR_IA32_SYSENTER_EIP: if (!canonical(val)) return -1; vmwrite(GUEST_SYSENTER_EIP, val); return 0;
	case MSR_IA32_PAT:
		for (int i = 0; i < 8; i++) {   /* types 0, 1, 4, 5, 6, 7 */
			uint8_t t = (uint8_t)(val >> (8 * i));
			if (t > 7 || t == 2 || t == 3) return -1;
		}
		vmwrite(GUEST_IA32_PAT, val);
		return 0;
	case MSR_DEBUGCTL:          return val ? -1 : 0;   /* no branch tracing for guests */
	case MSR_EFER: {
		uint64_t allowed = EFER_SCE | EFER_LME | EFER_LMA | EFER_NXE;
		if (val & ~allowed) return -1;
		/* LME cannot change with paging on; LMA is the CPU's to set */
		if ((v->cr0 & (1ULL << 31)) && ((val ^ v->efer) & EFER_LME)) return -1;
		v->efer = (val & ~EFER_LMA) | (v->efer & EFER_LMA);
		vmx_efer_update(v);
		return 0;
	}
	case MSR_FS_BASE_: if (!canonical(val)) return -1; vmwrite(GUEST_ES_BASE + 2 * SEG_FS, val); return 0;
	case MSR_GS_BASE_: if (!canonical(val)) return -1; vmwrite(GUEST_ES_BASE + 2 * SEG_GS, val); return 0;
	case MSR_TSC:        vmwrite(TSC_OFFSET, val - rdtsc()); return 0;
	case MSR_APIC_BASE:  v->apic_base = val; return 0;
	case MSR_BIOS_SIGN_ID: return 0;   /* a microcode update check */
	case MSR_MCG_STATUS: v->mcg_status = val; return 0;
	case MSR_MISC_ENABLE: v->misc_enable = val; return 0;
	case MSR_KVM_WALL_CLOCK:
	case MSR_KVM_SYSTEM_TIME:
	case MSR_KVM_WALL_CLOCK_NEW:
	case MSR_KVM_SYSTEM_TIME_NEW:
		return val ? -1 : 0;   /* off is all there is */
	case MSR_MTRR_DEF_TYPE: v->mtrr_def = val; return 0;
	}
	if (idx >= MSR_MTRR_PHYS0 && idx < MSR_MTRR_PHYS0 + 2 * MTRR_VAR) {
		v->mtrr_var[idx - MSR_MTRR_PHYS0] = val;
		return 0;
	}
	if ((s = fixed_slot(idx)) >= 0) { v->mtrr_fix[s] = val; return 0; }
	uint64_t *mce = mce_reg(v, idx);
	if (mce) { *mce = val; return 0; }
	if ((s = switched_slot(idx)) >= 0) {
		if (idx != MSR_STAR && idx != MSR_SFMASK && !canonical(val)) return -1;
		v->msr_guest[s].data = val;
		return 0;
	}
	return msr_unknown(idx, 1);
}

/* KVM_GET_MSR_INDEX_LIST: nmsrs in, the list out, E2BIG if it does not fit */
int kvm_msr_index_list(struct kvm_msr_list *ul)
{
	struct kvm_msr_list h;
	uint32_t n = sizeof(saved) / sizeof(saved[0]);
	if (copy_from_user(&h, ul, sizeof(h))) return -EFAULT;
	int big = h.nmsrs < n;
	h.nmsrs = n;
	if (copy_to_user(ul, &h, sizeof(h))) return -EFAULT;
	if (big) return -E2BIG;
	return copy_to_user(ul->indices, saved, sizeof(saved)) ? -EFAULT : 0;
}
