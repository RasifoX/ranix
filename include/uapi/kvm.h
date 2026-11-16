/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef UAPI_KVM_H
#define UAPI_KVM_H

/* /dev/kvm (arch/x86_64/kvm.c): the KVM ioctl interface, so a virtual machine
 * monitor built for Linux (QEMU, test programs) runs unchanged. Only the part
 * Ranix implements is here; an ioctl that is not here fails with ENOTTY.
 *
 * Provenance: names, numbers and structure layouts follow the public KVM API
 * description (Linux Documentation/virt/kvm/api.rst), read as an interface
 * definition: a binary that was compiled against Linux's <linux/kvm.h> must see
 * the same numbers and the same bytes. No Linux source was read or copied.
 *
 * The includer provides uint8_t..uint64_t (<stdint.h>, or <uapi/types.h> in
 * the kernel). */

#define KVM_API_VERSION 12

#define KVMIO 0xAE
#define KVM_IOC_NONE  0U
#define KVM_IOC_WRITE 1U
#define KVM_IOC_READ  2U
#define KVM_IOC(dir, nr, size) (((dir) << 30) | ((uint32_t)(size) << 16) | (KVMIO << 8) | (nr))
#define KVM_IO(nr)          KVM_IOC(KVM_IOC_NONE, nr, 0)
#define KVM_IOR(nr, t)      KVM_IOC(KVM_IOC_READ, nr, sizeof(t))
#define KVM_IOW(nr, t)      KVM_IOC(KVM_IOC_WRITE, nr, sizeof(t))
#define KVM_IOWR(nr, t)     KVM_IOC(KVM_IOC_READ | KVM_IOC_WRITE, nr, sizeof(t))

struct kvm_userspace_memory_region {
	uint32_t slot;
	uint32_t flags;
	uint64_t guest_phys_addr;
	uint64_t memory_size;     /* bytes; 0 deletes the slot */
	uint64_t userspace_addr;  /* start of the memory in the caller's address space */
};
#define KVM_MEM_LOG_DIRTY_PAGES (1U << 0)
#define KVM_MEM_READONLY        (1U << 1)

struct kvm_regs {
	uint64_t rax, rbx, rcx, rdx;
	uint64_t rsi, rdi, rsp, rbp;
	uint64_t r8,  r9,  r10, r11;
	uint64_t r12, r13, r14, r15;
	uint64_t rip, rflags;
};

struct kvm_segment {
	uint64_t base;
	uint32_t limit;
	uint16_t selector;
	uint8_t  type;
	uint8_t  present, dpl, db, s, l, g, avl;
	uint8_t  unusable;
	uint8_t  padding;
};

struct kvm_dtable {
	uint64_t base;
	uint16_t limit;
	uint16_t padding[3];
};

#define KVM_NR_INTERRUPTS 256
struct kvm_sregs {
	struct kvm_segment cs, ds, es, fs, gs, ss;
	struct kvm_segment tr, ldt;
	struct kvm_dtable gdt, idt;
	uint64_t cr0, cr2, cr3, cr4, cr8;
	uint64_t efer;
	uint64_t apic_base;
	uint64_t interrupt_bitmap[(KVM_NR_INTERRUPTS + 63) / 64];
};

struct kvm_cpuid_entry2 {
	uint32_t function;
	uint32_t index;
	uint32_t flags;
	uint32_t eax, ebx, ecx, edx;
	uint32_t padding[3];
};
#define KVM_CPUID_FLAG_SIGNIFCANT_INDEX (1U << 0)   /* the spelling is the API's */

struct kvm_cpuid2 {
	uint32_t nent;
	uint32_t padding;
	struct kvm_cpuid_entry2 entries[];
};

struct kvm_msr_entry {
	uint32_t index;
	uint32_t reserved;
	uint64_t data;
};

struct kvm_msrs {
	uint32_t nmsrs;
	uint32_t pad;
	struct kvm_msr_entry entries[];
};

struct kvm_msr_list {
	uint32_t nmsrs;
	uint32_t indices[];
};

struct kvm_interrupt {
	uint32_t irq;
};

struct kvm_fpu {
	uint8_t  fpr[8][16];
	uint16_t fcw;
	uint16_t fsw;
	uint8_t  ftwx;      /* in fxsave format */
	uint8_t  pad1;
	uint16_t last_opcode;
	uint64_t last_ip;
	uint64_t last_dp;
	uint8_t  xmm[16][16];
	uint32_t mxcsr;
	uint32_t pad2;
};

struct kvm_xsave {
	uint32_t region[1024];   /* the XSAVE area, non-compacted */
};

struct kvm_xcr {
	uint32_t xcr;
	uint32_t reserved;
	uint64_t value;
};

struct kvm_xcrs {
	uint32_t nr_xcrs;
	uint32_t flags;
	struct kvm_xcr xcrs[16];
	uint64_t padding[16];
};

struct kvm_vcpu_events {
	struct {
		uint8_t  injected;
		uint8_t  nr;
		uint8_t  has_error_code;
		uint8_t  pending;
		uint32_t error_code;
	} exception;
	struct {
		uint8_t injected;
		uint8_t nr;
		uint8_t soft;
		uint8_t shadow;
	} interrupt;
	struct {
		uint8_t injected;
		uint8_t pending;
		uint8_t masked;
		uint8_t pad;
	} nmi;
	uint32_t sipi_vector;
	uint32_t flags;
	struct {
		uint8_t smm;
		uint8_t pending;
		uint8_t smm_inside_nmi;
		uint8_t latched_init;
	} smi;
	struct {
		uint8_t pending;
	} triple_fault;
	uint8_t  reserved[26];
	uint8_t  exception_has_payload;
	uint64_t exception_payload;
};
#define KVM_VCPUEVENT_VALID_NMI_PENDING 0x00000001
#define KVM_VCPUEVENT_VALID_SIPI_VECTOR 0x00000002
#define KVM_VCPUEVENT_VALID_SHADOW      0x00000004
#define KVM_X86_SHADOW_INT_MOV_SS 0x01
#define KVM_X86_SHADOW_INT_STI    0x02

struct kvm_debugregs {
	uint64_t db[4];
	uint64_t dr6;
	uint64_t dr7;
	uint64_t flags;
	uint64_t reserved[9];
};

struct kvm_mp_state {
	uint32_t mp_state;
};
#define KVM_MP_STATE_RUNNABLE      0
#define KVM_MP_STATE_UNINITIALIZED 1
#define KVM_MP_STATE_INIT_RECEIVED 2
#define KVM_MP_STATE_HALTED        3
#define KVM_MP_STATE_SIPI_RECEIVED 4

struct kvm_clock_data {
	uint64_t clock;
	uint32_t flags;
	uint32_t pad0;
	uint64_t realtime;
	uint64_t host_tsc;
	uint32_t pad[4];
};

struct kvm_enable_cap {
	uint32_t cap;
	uint32_t flags;
	uint64_t args[4];
	uint8_t  pad[64];
};

struct kvm_ioeventfd {
	uint64_t datamatch;
	uint64_t addr;
	uint32_t len;
	int32_t  fd;
	uint32_t flags;
	uint8_t  pad[36];
};
#define KVM_IOEVENTFD_FLAG_DATAMATCH (1U << 0)
#define KVM_IOEVENTFD_FLAG_PIO       (1U << 1)
#define KVM_IOEVENTFD_FLAG_DEASSIGN  (1U << 2)

struct kvm_lapic_state {
	char regs[0x400];   /* the xAPIC register page: each register at its MMIO offset */
};

struct kvm_tpr_access_ctl {
	uint32_t enabled;
	uint32_t flags;
	uint32_t reserved[8];
};

struct kvm_vapic_addr {
	uint64_t vapic_addr;
};

struct kvm_irq_level {
	union {
		uint32_t irq;      /* GSI */
		int32_t  status;   /* KVM_IRQ_LINE_STATUS: 1 delivered */
	};
	uint32_t level;
};

struct kvm_msi {
	uint32_t address_lo;
	uint32_t address_hi;
	uint32_t data;
	uint32_t flags;
	uint32_t devid;
	uint8_t  pad[12];
};

#define KVM_IRQ_ROUTING_IRQCHIP 1
#define KVM_IRQ_ROUTING_MSI     2
struct kvm_irq_routing_entry {
	uint32_t gsi;
	uint32_t type;
	uint32_t flags;
	uint32_t pad;
	union {
		struct { uint32_t irqchip, pin; } irqchip;
		struct { uint32_t address_lo, address_hi, data, devid; } msi;
		uint32_t pad[8];
	} u;
};

struct kvm_irq_routing {
	uint32_t nr;
	uint32_t flags;
	struct kvm_irq_routing_entry entries[];
};

struct kvm_irqfd {
	uint32_t fd;
	uint32_t gsi;
	uint32_t flags;
	uint32_t resamplefd;
	uint8_t  pad[16];
};
#define KVM_IRQFD_FLAG_DEASSIGN (1U << 0)
#define KVM_IRQFD_FLAG_RESAMPLE (1U << 1)

/* kvm_run.exit_reason */
#define KVM_EXIT_UNKNOWN          0
#define KVM_EXIT_EXCEPTION        1
#define KVM_EXIT_IO               2
#define KVM_EXIT_HYPERCALL        3
#define KVM_EXIT_DEBUG            4
#define KVM_EXIT_HLT              5
#define KVM_EXIT_MMIO             6
#define KVM_EXIT_IRQ_WINDOW_OPEN  7
#define KVM_EXIT_SHUTDOWN         8
#define KVM_EXIT_FAIL_ENTRY       9
#define KVM_EXIT_INTR             10
#define KVM_EXIT_SET_TPR          11
#define KVM_EXIT_INTERNAL_ERROR   17
#define KVM_EXIT_IOAPIC_EOI       26

#define KVM_EXIT_IO_IN  0
#define KVM_EXIT_IO_OUT 1

#define KVM_INTERNAL_ERROR_EMULATION 1
#define KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON 4

/* The page userspace maps from a vcpu fd at offset 0 (KVM_GET_VCPU_MMAP_SIZE
 * bytes): KVM_RUN's in and out parameters. */
struct kvm_run {
	/* in */
	uint8_t  request_interrupt_window;
	uint8_t  immediate_exit;
	uint8_t  padding1[6];
	/* out */
	uint32_t exit_reason;
	uint8_t  ready_for_interrupt_injection;
	uint8_t  if_flag;
	uint16_t flags;
	/* in (pre_kvm_run), out (post_kvm_run) */
	uint64_t cr8;
	uint64_t apic_base;
	union {
		struct {
			uint64_t hardware_exit_reason;
		} hw;
		struct {
			uint64_t hardware_entry_failure_reason;
			uint32_t cpu;
		} fail_entry;
		struct {
			uint32_t exception;
			uint32_t error_code;
		} ex;
		struct {
			uint8_t  direction;    /* KVM_EXIT_IO_IN / _OUT */
			uint8_t  size;         /* bytes per access: 1, 2, 4 */
			uint16_t port;
			uint32_t count;
			uint64_t data_offset;  /* from the start of this structure */
		} io;
		struct {
			uint64_t phys_addr;
			uint8_t  data[8];
			uint32_t len;
			uint8_t  is_write;
		} mmio;
		struct {
			uint32_t suberror;
			uint32_t ndata;
			uint64_t data[16];
		} internal;
		struct {
			uint8_t vector;   /* a level-triggered vector the guest ended: for userspace's IOAPIC */
		} eoi;
		char padding[256];
	};
	uint64_t kvm_valid_regs;
	uint64_t kvm_dirty_regs;
	union {
		char padding[2048];
	} s;
};

/* ioctls on /dev/kvm */
#define KVM_GET_API_VERSION        KVM_IO(0x00)
#define KVM_CREATE_VM              KVM_IO(0x01)   /* arg: machine type, 0 */
#define KVM_GET_MSR_INDEX_LIST     KVM_IOWR(0x02, struct kvm_msr_list)
#define KVM_CHECK_EXTENSION        KVM_IO(0x03)   /* also on a VM fd */
#define KVM_GET_VCPU_MMAP_SIZE     KVM_IO(0x04)
#define KVM_GET_SUPPORTED_CPUID    KVM_IOWR(0x05, struct kvm_cpuid2)
#define KVM_X86_GET_MCE_CAP_SUPPORTED KVM_IOR(0x9d, uint64_t)

/* ioctls on a VM fd */
#define KVM_CREATE_VCPU            KVM_IO(0x41)   /* arg: vcpu id */
#define KVM_SET_USER_MEMORY_REGION KVM_IOW(0x46, struct kvm_userspace_memory_region)
/* the pages of a KVM_MEM_LOG_DIRTY_PAGES slot written since the last call, one
 * bit each (bitmap of ALIGN(pages, 64) bits); the call starts the next round */
struct kvm_dirty_log {
	uint32_t slot;
	uint32_t padding1;
	uint64_t dirty_bitmap;   /* user pointer */
};
#define KVM_GET_DIRTY_LOG KVM_IOW(0x42, struct kvm_dirty_log)
#define KVM_SET_TSS_ADDR           KVM_IO(0x47)
#define KVM_CREATE_IRQCHIP         KVM_IO(0x60)
#define KVM_IRQ_LINE               KVM_IOW(0x61, struct kvm_irq_level)
#define KVM_IRQ_LINE_STATUS        KVM_IOWR(0x67, struct kvm_irq_level)
#define KVM_SET_GSI_ROUTING        KVM_IOW(0x6a, struct kvm_irq_routing)
#define KVM_IRQFD                  KVM_IOW(0x76, struct kvm_irqfd)
#define KVM_SIGNAL_MSI             KVM_IOW(0xa5, struct kvm_msi)
#define KVM_SET_IDENTITY_MAP_ADDR  KVM_IOW(0x48, uint64_t)
#define KVM_IOEVENTFD              KVM_IOW(0x79, struct kvm_ioeventfd)
#define KVM_SET_CLOCK              KVM_IOW(0x7b, struct kvm_clock_data)
#define KVM_GET_CLOCK              KVM_IOR(0x7c, struct kvm_clock_data)
#define KVM_ENABLE_CAP             KVM_IOW(0xa3, struct kvm_enable_cap)   /* also on a vcpu fd */

/* ioctls on a vcpu fd */
#define KVM_RUN                    KVM_IO(0x80)
#define KVM_GET_REGS               KVM_IOR(0x81, struct kvm_regs)
#define KVM_SET_REGS               KVM_IOW(0x82, struct kvm_regs)
#define KVM_GET_SREGS              KVM_IOR(0x83, struct kvm_sregs)
#define KVM_SET_SREGS              KVM_IOW(0x84, struct kvm_sregs)
#define KVM_INTERRUPT              KVM_IOW(0x86, struct kvm_interrupt)
#define KVM_GET_MSRS               KVM_IOWR(0x88, struct kvm_msrs)
#define KVM_SET_MSRS               KVM_IOW(0x89, struct kvm_msrs)
#define KVM_GET_FPU                KVM_IOR(0x8c, struct kvm_fpu)
#define KVM_SET_FPU                KVM_IOW(0x8d, struct kvm_fpu)
#define KVM_GET_LAPIC              KVM_IOR(0x8e, struct kvm_lapic_state)
#define KVM_SET_LAPIC              KVM_IOW(0x8f, struct kvm_lapic_state)
#define KVM_SET_CPUID2             KVM_IOW(0x90, struct kvm_cpuid2)
#define KVM_GET_CPUID2             KVM_IOWR(0x91, struct kvm_cpuid2)
#define KVM_TPR_ACCESS_REPORTING   KVM_IOWR(0x92, struct kvm_tpr_access_ctl)
#define KVM_SET_VAPIC_ADDR         KVM_IOW(0x93, struct kvm_vapic_addr)
#define KVM_GET_MP_STATE           KVM_IOR(0x98, struct kvm_mp_state)
#define KVM_SET_MP_STATE           KVM_IOW(0x99, struct kvm_mp_state)
#define KVM_NMI                    KVM_IO(0x9a)
#define KVM_X86_SETUP_MCE          KVM_IOW(0x9c, uint64_t)
#define KVM_GET_VCPU_EVENTS        KVM_IOR(0x9f, struct kvm_vcpu_events)
#define KVM_SET_VCPU_EVENTS        KVM_IOW(0xa0, struct kvm_vcpu_events)
#define KVM_GET_DEBUGREGS          KVM_IOR(0xa1, struct kvm_debugregs)
#define KVM_SET_DEBUGREGS          KVM_IOW(0xa2, struct kvm_debugregs)
#define KVM_SET_TSC_KHZ            KVM_IO(0xa2)
#define KVM_GET_TSC_KHZ            KVM_IO(0xa3)
#define KVM_GET_XSAVE              KVM_IOR(0xa4, struct kvm_xsave)
#define KVM_SET_XSAVE              KVM_IOW(0xa5, struct kvm_xsave)
#define KVM_GET_XCRS               KVM_IOR(0xa6, struct kvm_xcrs)
#define KVM_SET_XCRS               KVM_IOW(0xa7, struct kvm_xcrs)

/* KVM_CHECK_EXTENSION */
#define KVM_CAP_IRQCHIP                     0
#define KVM_CAP_HLT                         1
#define KVM_CAP_USER_MEMORY                 3
#define KVM_CAP_SET_TSS_ADDR                4
#define KVM_CAP_EXT_CPUID                   7
#define KVM_CAP_NR_VCPUS                    9
#define KVM_CAP_NR_MEMSLOTS                 10
#define KVM_CAP_MP_STATE                    14
#define KVM_CAP_DESTROY_MEMORY_REGION_WORKS 21
#define KVM_CAP_USER_NMI                    22
#define KVM_CAP_IRQ_ROUTING                 25
#define KVM_CAP_IRQ_INJECT_STATUS           26
#define KVM_CAP_IRQFD                       32
#define KVM_CAP_MCE                         31
#define KVM_CAP_JOIN_MEMORY_REGIONS_WORKS   30
#define KVM_CAP_IOEVENTFD                   36
#define KVM_CAP_SET_IDENTITY_MAP_ADDR       37
#define KVM_CAP_ADJUST_CLOCK                39
#define KVM_CAP_INTERNAL_ERROR_DATA         40
#define KVM_CAP_VCPU_EVENTS                 41
#define KVM_CAP_DEBUGREGS                   50
#define KVM_CAP_X86_ROBUST_SINGLESTEP       51
#define KVM_CAP_ENABLE_CAP                  54
#define KVM_CAP_XSAVE                       55
#define KVM_CAP_XCRS                        56
#define KVM_CAP_GET_TSC_KHZ                 61
#define KVM_CAP_SIGNAL_MSI                  77
#define KVM_CAP_MAX_VCPUS                   66
#define KVM_CAP_SPLIT_IRQCHIP               121
#define KVM_CAP_IOEVENTFD_ANY_LENGTH        122
#define KVM_CAP_IMMEDIATE_EXIT              136

#endif
