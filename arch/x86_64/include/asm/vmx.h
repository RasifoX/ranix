/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef ASM_VMX_H
#define ASM_VMX_H

/* Intel VT-x: MSRs, VMCS field encodings, control bits and exit reasons, and
 * the VMX instructions as inline functions.
 *
 * Provenance: every number here is from the Intel 64 and IA-32 Architectures
 * Software Developer's Manual, Volume 3 (chapters 24-28 and appendices A-C:
 * VMX capability MSRs, VMCS encodings, basic exit reasons). The names follow
 * the manual's wording. */

#include <uapi/types.h>

/* ── MSRs ─────────────────────────────────────────────────── */
#define MSR_IA32_FEATURE_CONTROL   0x3A
#define  FEATURE_CONTROL_LOCK      (1ULL << 0)
#define  FEATURE_CONTROL_VMX       (1ULL << 2)   /* VMXON outside SMX */
#define MSR_IA32_SYSENTER_CS       0x174
#define MSR_IA32_SYSENTER_ESP      0x175
#define MSR_IA32_SYSENTER_EIP      0x176
#define MSR_IA32_PAT               0x277
#define MSR_IA32_VMX_BASIC         0x480
#define MSR_IA32_VMX_PINBASED      0x481
#define MSR_IA32_VMX_PROCBASED     0x482
#define MSR_IA32_VMX_EXIT          0x483
#define MSR_IA32_VMX_ENTRY         0x484
#define MSR_IA32_VMX_MISC          0x485
#define MSR_IA32_VMX_CR0_FIXED0    0x486
#define MSR_IA32_VMX_CR0_FIXED1    0x487
#define MSR_IA32_VMX_CR4_FIXED0    0x488
#define MSR_IA32_VMX_CR4_FIXED1    0x489
#define MSR_IA32_VMX_PROCBASED2    0x48B
#define MSR_IA32_VMX_EPT_VPID_CAP  0x48C
#define MSR_IA32_VMX_TRUE_PINBASED 0x48D
#define MSR_IA32_VMX_TRUE_PROCBASED 0x48E
#define MSR_IA32_VMX_TRUE_EXIT     0x48F
#define MSR_IA32_VMX_TRUE_ENTRY    0x490
#define  VMX_BASIC_TRUE_CTLS       (1ULL << 55)
#define MSR_EFER                   0xC0000080
#define  EFER_SCE                  (1ULL << 0)
#define  EFER_LME                  (1ULL << 8)
#define  EFER_LMA                  (1ULL << 10)
#define  EFER_NXE                  (1ULL << 11)
#define MSR_FS_BASE_               0xC0000100
#define MSR_GS_BASE_               0xC0000101

/* EPT/VPID capability bits (IA32_VMX_EPT_VPID_CAP) */
#define EPT_CAP_WALK4              (1ULL << 6)
#define EPT_CAP_WB                 (1ULL << 14)
#define EPT_CAP_INVEPT             (1ULL << 20)
#define EPT_CAP_INVEPT_SINGLE      (1ULL << 25)
#define EPT_CAP_INVEPT_ALL         (1ULL << 26)

/* ── control bits ─────────────────────────────────────────── */
#define PIN_EXT_INTR_EXIT          (1U << 0)
#define PIN_NMI_EXIT               (1U << 3)
#define PIN_VIRTUAL_NMIS           (1U << 5)

#define CPU_INTR_WINDOW_EXIT       (1U << 2)
#define CPU_HLT_EXIT               (1U << 7)
#define CPU_INVLPG_EXIT            (1U << 9)
#define CPU_MWAIT_EXIT             (1U << 10)
#define CPU_RDPMC_EXIT             (1U << 11)
#define CPU_RDTSC_EXIT             (1U << 12)
#define CPU_CR3_LOAD_EXIT          (1U << 15)
#define CPU_CR3_STORE_EXIT         (1U << 16)
#define CPU_CR8_LOAD_EXIT          (1U << 19)
#define CPU_CR8_STORE_EXIT         (1U << 20)
#define CPU_TPR_SHADOW             (1U << 21)
#define CPU_NMI_WINDOW_EXIT        (1U << 22)
#define CPU_MOV_DR_EXIT            (1U << 23)
#define CPU_UNCOND_IO_EXIT         (1U << 24)
#define CPU_USE_IO_BITMAPS         (1U << 25)
#define CPU_MONITOR_TRAP_FLAG      (1U << 27)
#define CPU_USE_MSR_BITMAPS        (1U << 28)
#define CPU_MONITOR_EXIT           (1U << 29)
#define CPU_PAUSE_EXIT             (1U << 30)
#define CPU_SECONDARY              (1U << 31)

#define CPU2_VIRT_APIC_ACCESS      (1U << 0)
#define CPU2_EPT                   (1U << 1)
#define CPU2_DESC_TABLE_EXIT       (1U << 2)
#define CPU2_RDTSCP                (1U << 3)
#define CPU2_VPID                  (1U << 5)
#define CPU2_WBINVD_EXIT           (1U << 6)
#define CPU2_UNRESTRICTED_GUEST    (1U << 7)
#define CPU2_INVPCID               (1U << 12)
#define CPU2_XSAVES                (1U << 20)

#define EXIT_SAVE_DEBUG            (1U << 2)
#define EXIT_HOST_ADDR64           (1U << 9)
#define EXIT_ACK_INTR              (1U << 15)
#define EXIT_SAVE_PAT              (1U << 18)
#define EXIT_LOAD_PAT              (1U << 19)
#define EXIT_SAVE_EFER             (1U << 20)
#define EXIT_LOAD_EFER             (1U << 21)

#define ENTRY_LOAD_DEBUG           (1U << 2)
#define ENTRY_IA32E_GUEST          (1U << 9)
#define ENTRY_LOAD_PAT             (1U << 14)
#define ENTRY_LOAD_EFER            (1U << 15)

/* ── VMCS fields ──────────────────────────────────────────── */
enum vmcs_field {
	GUEST_ES_SELECTOR       = 0x0800,
	GUEST_CS_SELECTOR       = 0x0802,
	GUEST_SS_SELECTOR       = 0x0804,
	GUEST_DS_SELECTOR       = 0x0806,
	GUEST_FS_SELECTOR       = 0x0808,
	GUEST_GS_SELECTOR       = 0x080A,
	GUEST_LDTR_SELECTOR     = 0x080C,
	GUEST_TR_SELECTOR       = 0x080E,
	HOST_ES_SELECTOR        = 0x0C00,
	HOST_CS_SELECTOR        = 0x0C02,
	HOST_SS_SELECTOR        = 0x0C04,
	HOST_DS_SELECTOR        = 0x0C06,
	HOST_FS_SELECTOR        = 0x0C08,
	HOST_GS_SELECTOR        = 0x0C0A,
	HOST_TR_SELECTOR        = 0x0C0C,

	IO_BITMAP_A             = 0x2000,
	IO_BITMAP_B             = 0x2002,
	MSR_BITMAP              = 0x2004,
	VM_EXIT_MSR_STORE_ADDR  = 0x2006,
	VM_EXIT_MSR_LOAD_ADDR   = 0x2008,
	VM_ENTRY_MSR_LOAD_ADDR  = 0x200A,
	TSC_OFFSET              = 0x2010,
	EPT_POINTER             = 0x201A,
	GUEST_PHYSICAL_ADDRESS  = 0x2400,
	VMCS_LINK_POINTER       = 0x2800,
	GUEST_IA32_DEBUGCTL     = 0x2802,
	GUEST_IA32_PAT          = 0x2804,
	GUEST_IA32_EFER         = 0x2806,
	GUEST_PDPTE0            = 0x280A,
	HOST_IA32_PAT           = 0x2C00,
	HOST_IA32_EFER          = 0x2C02,

	PIN_BASED_CONTROLS      = 0x4000,
	CPU_BASED_CONTROLS      = 0x4002,
	EXCEPTION_BITMAP        = 0x4004,
	PF_ERROR_CODE_MASK      = 0x4006,
	PF_ERROR_CODE_MATCH     = 0x4008,
	CR3_TARGET_COUNT        = 0x400A,
	VM_EXIT_CONTROLS        = 0x400C,
	VM_EXIT_MSR_STORE_COUNT = 0x400E,
	VM_EXIT_MSR_LOAD_COUNT  = 0x4010,
	VM_ENTRY_CONTROLS       = 0x4012,
	VM_ENTRY_MSR_LOAD_COUNT = 0x4014,
	VM_ENTRY_INTR_INFO      = 0x4016,
	VM_ENTRY_EXCEPTION_ERROR_CODE = 0x4018,
	VM_ENTRY_INSTRUCTION_LEN = 0x401A,
	TPR_THRESHOLD           = 0x401C,
	SECONDARY_CONTROLS      = 0x401E,
	VM_INSTRUCTION_ERROR    = 0x4400,
	VM_EXIT_REASON          = 0x4402,
	VM_EXIT_INTR_INFO       = 0x4404,
	VM_EXIT_INTR_ERROR_CODE = 0x4406,
	IDT_VECTORING_INFO      = 0x4408,
	IDT_VECTORING_ERROR_CODE = 0x440A,
	VM_EXIT_INSTRUCTION_LEN = 0x440C,
	VMX_INSTRUCTION_INFO    = 0x440E,
	GUEST_ES_LIMIT          = 0x4800,   /* +2 per segment: ES CS SS DS FS GS LDTR TR */
	GUEST_GDTR_LIMIT        = 0x4810,
	GUEST_IDTR_LIMIT        = 0x4812,
	GUEST_ES_AR             = 0x4814,   /* +2 per segment, same order */
	GUEST_INTERRUPTIBILITY  = 0x4824,
	GUEST_ACTIVITY_STATE    = 0x4826,
	GUEST_SYSENTER_CS       = 0x482A,
	HOST_IA32_SYSENTER_CS   = 0x4C00,

	CR0_GUEST_HOST_MASK     = 0x6000,
	CR4_GUEST_HOST_MASK     = 0x6002,
	CR0_READ_SHADOW         = 0x6004,
	CR4_READ_SHADOW         = 0x6006,
	EXIT_QUALIFICATION      = 0x6400,
	GUEST_LINEAR_ADDRESS    = 0x640A,
	GUEST_CR0               = 0x6800,
	GUEST_CR3               = 0x6802,
	GUEST_CR4               = 0x6804,
	GUEST_ES_BASE           = 0x6806,   /* +2 per segment, same order */
	GUEST_GDTR_BASE         = 0x6816,
	GUEST_IDTR_BASE         = 0x6818,
	GUEST_DR7               = 0x681A,
	GUEST_RSP               = 0x681C,
	GUEST_RIP               = 0x681E,
	GUEST_RFLAGS            = 0x6820,
	GUEST_PENDING_DBG       = 0x6822,
	GUEST_SYSENTER_ESP      = 0x6824,
	GUEST_SYSENTER_EIP      = 0x6826,
	HOST_CR0                = 0x6C00,
	HOST_CR3                = 0x6C02,
	HOST_CR4                = 0x6C04,
	HOST_FS_BASE            = 0x6C06,
	HOST_GS_BASE            = 0x6C08,
	HOST_TR_BASE            = 0x6C0A,
	HOST_GDTR_BASE          = 0x6C0C,
	HOST_IDTR_BASE          = 0x6C0E,
	HOST_IA32_SYSENTER_ESP  = 0x6C10,
	HOST_IA32_SYSENTER_EIP  = 0x6C12,
	HOST_RSP                = 0x6C14,
	HOST_RIP                = 0x6C16,
};

/* segment index for the GUEST_ES_* + 2*i fields */
enum { SEG_ES, SEG_CS, SEG_SS, SEG_DS, SEG_FS, SEG_GS, SEG_LDTR, SEG_TR };

/* access-rights bit that marks a segment register unusable */
#define AR_UNUSABLE (1U << 16)

/* ── basic exit reasons ───────────────────────────────────── */
#define EXIT_EXCEPTION_NMI   0
#define EXIT_EXTERNAL_INTR   1
#define EXIT_TRIPLE_FAULT    2
#define EXIT_INIT            3
#define EXIT_INTR_WINDOW     7
#define EXIT_NMI_WINDOW      8
#define EXIT_CPUID           10
#define EXIT_HLT             12
#define EXIT_INVD            13
#define EXIT_RDPMC           15
#define EXIT_VMCALL          18
#define EXIT_CR_ACCESS       28
#define EXIT_IO              30
#define EXIT_RDMSR           31
#define EXIT_WRMSR           32
#define EXIT_ENTRY_BAD_GUEST 33
#define EXIT_ENTRY_BAD_MSR   34
#define EXIT_MWAIT           36
#define EXIT_MONITOR         39
#define EXIT_PAUSE           40
#define EXIT_EPT_VIOLATION   48
#define EXIT_EPT_MISCONFIG   49
#define EXIT_WBINVD          54
#define EXIT_XSETBV          55
#define EXIT_REASON_ENTRY_FAILED (1U << 31)

/* VM-exit / VM-entry interruption information */
#define INTR_INFO_VALID      (1U << 31)
#define INTR_INFO_ERRCODE    (1U << 11)
#define INTR_TYPE_EXT        (0U << 8)
#define INTR_TYPE_NMI        (2U << 8)
#define INTR_TYPE_HW_EXC     (3U << 8)
#define INTR_TYPE_SW_INT     (4U << 8)
#define INTR_TYPE_MASK       (7U << 8)

/* guest interruptibility state */
#define INTERRUPTIBILITY_STI   (1U << 0)
#define INTERRUPTIBILITY_MOVSS (1U << 1)

/* ── instructions ─────────────────────────────────────────── */
static inline uint64_t vmx_rdmsr(uint32_t msr)
{
	uint32_t lo, hi;
	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t)hi << 32) | lo;
}

static inline void vmx_wrmsr(uint32_t msr, uint64_t v)
{
	__asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

/* VMX instructions report failure in CF (no current VMCS) or ZF (an error
 * number in VM_INSTRUCTION_ERROR); either is returned as 1. */
static inline int vmx_on(uint64_t phys)
{
	uint8_t bad;
	__asm__ volatile("vmxon %1; setbe %0" : "=q"(bad) : "m"(phys) : "cc", "memory");
	return bad;
}

static inline int vmx_ptrld(uint64_t phys)
{
	uint8_t bad;
	__asm__ volatile("vmptrld %1; setbe %0" : "=q"(bad) : "m"(phys) : "cc", "memory");
	return bad;
}

static inline int vmx_clear(uint64_t phys)
{
	uint8_t bad;
	__asm__ volatile("vmclear %1; setbe %0" : "=q"(bad) : "m"(phys) : "cc", "memory");
	return bad;
}

static inline uint64_t vmread(uint32_t field)
{
	uint64_t v;
	__asm__ volatile("vmread %1, %0" : "=rm"(v) : "r"((uint64_t)field) : "cc");
	return v;
}

static inline void vmwrite(uint32_t field, uint64_t v)
{
	__asm__ volatile("vmwrite %1, %0" :: "r"((uint64_t)field), "rm"(v) : "cc");
}

/* INVEPT type 1: drop the guest-physical translations cached for one EPTP */
static inline void vmx_invept_single(uint64_t eptp)
{
	struct { uint64_t eptp, zero; } d = { eptp, 0 };
	__asm__ volatile("invept %0, %1" :: "m"(d), "r"((uint64_t)1) : "cc", "memory");
}

#endif
