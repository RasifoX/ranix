/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef ASM_KVM_H
#define ASM_KVM_H

/* KVM inside the kernel: a VM, its vcpus and memory slots, shared by the
 * /dev/kvm interface (kvm.c), guest memory through EPT (kvm_ept.c) and the VT-x
 * hardware layer (vmx.c, vmx_entry.S). */

#include <uapi/types.h>
#include <uapi/kvm.h>
#include <kernel/sync.h>
#include <kernel/smp.h>
#include <kernel/mm/vm.h>

#define KVM_MAX_VCPUS 64
#define KVM_MAX_SLOTS 64

typedef struct kvm_slot {
	uint64_t gpa, size, hva;   /* size 0: the slot is free */
	uint32_t flags;
} kvm_slot_t;

#define KVM_MAX_IOEVENTFD 128

typedef struct kvm_ioev {
	uint64_t addr, datamatch;
	uint32_t len, flags;          /* len 0: any length */
	struct vfs_file *volatile file;   /* the eventfd; NULL: a free entry */
} kvm_ioev_t;

typedef struct kvm_vm {
	mutex_t     lock;        /* slots, vcpus[], every change to the EPT tables */
	vm_space_t *vs;          /* the creator's address space: slots name its addresses */
	uint32_t    refs;        /* the VM fd and each vcpu */
	uint64_t    ept_root;    /* physical PML4 of the EPT tables */
	uint64_t    eptp;
	kvm_slot_t  slots[KVM_MAX_SLOTS];
	volatile uint32_t slots_seq;   /* odd while slots[] changes: for kvm_slot_present */
	uint64_t    clock_off;
	kvm_ioev_t  ioev[KVM_MAX_IOEVENTFD];   /* KVM_SET_CLOCK: the VM's clock minus the host's monotonic */
	struct kvm_vcpu *vcpus[KVM_MAX_VCPUS];
	/* EPT translations a CPU may still cache. ept_gen moves when a mapping is
	 * taken away; a vcpu entering on a CPU whose cpu_gen is behind flushes it
	 * first (see kvm_vm_quiesce). */
	volatile uint64_t ept_gen;
	uint64_t    cpu_gen[MAX_CPUS];
} kvm_vm_t;

/* Guest general registers by x86 register number (rax rcx rdx rbx rsp rbp rsi
 * rdi r8..r15), so an exit qualification's register field indexes r[]. r[4] is
 * unused: the guest RSP lives in the VMCS. vmx_entry.S uses these offsets. */
typedef struct vmx_gregs {
	uint64_t r[16];
	uint64_t cr2;
} vmx_gregs_t;

enum { R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };

#define KVM_MAX_CPUID_ENTRIES 256
#define KVM_SWITCHED_MSRS 5   /* STAR LSTAR CSTAR SFMASK KERNEL_GS_BASE (kvm_msr.c) */

/* a decoded MMIO load or store (kvm_emul.c) */
typedef struct kvm_mmio {
	uint8_t  len;        /* instruction bytes */
	uint8_t  size;       /* access bytes */
	uint8_t  write;
	uint8_t  sign;       /* MOVSX: sign-extend the loaded value */
	uint8_t  reg_size;   /* bytes of the register a load writes */
	uint8_t  high8;      /* AH CH DH BH */
	int8_t   reg;        /* register number, -1: the immediate */
	uint64_t imm;
} kvm_mmio_t;

typedef struct kvm_vcpu {
	vmx_gregs_t g;
	kvm_vm_t   *vm;
	int         id;
	mutex_t     lock;        /* one ioctl at a time, as on Linux */
	uint64_t    vmcs;        /* physical page */
	uint64_t    run_page;    /* physical: struct kvm_run */
	uint64_t    pio_page;    /* physical: port I/O data, the page after kvm_run */
	struct kvm_run *run;
	uint8_t    *fpu;         /* guest x87/SSE/AVX state, XSAVE layout, one page */
	/* the guest's view of the registers the VMCS shadows or the host owns */
	uint64_t    cr0, cr4, efer, xcr0, apic_base;
	uint32_t    pio_in;      /* an IN is waiting for userspace's data: its size */
	uint64_t    prefault;    /* guest-physical page to map before the next entry, or ~0 */
	kvm_mmio_t  mmio;        /* a load waiting for userspace's data, when mmio_pending */
	uint8_t     mmio_pending;
	int         irq_pending; /* KVM_INTERRUPT vector not yet delivered, or -1 */
	uint8_t     nmi_pending; /* KVM_NMI not yet delivered */
	uint32_t    cpu_ctl;     /* primary controls as last written (interrupt/NMI windows) */
	struct kvm_cpuid_entry2 *cpuid;   /* KVM_SET_CPUID2's table, NULL before */
	uint32_t    ncpuid;
	/* MSRs (kvm_msr.c): the switched ones in the VM-entry load / VM-exit store
	 * area, the host's in the VM-exit load area (one page, msr_page) */
	uint64_t    msr_page;
	struct kvm_msr_entry *msr_guest, *msr_host;
	uint32_t    msr_count;   /* switched MSRs as last written to the VMCS */
	uint64_t    misc_enable, mtrr_def, mtrr_var[16], mtrr_fix[11];
	uint64_t    dr[4], dr6;  /* the guest's debug registers (DR7 is in the VMCS) */
	/* kvm_vm_quiesce: the CPU running this vcpu with the VMCS loaded (-1 none)
	 * and the ept_gen it flushed for before its last entry */
	volatile int      guest_cpu;
	volatile uint64_t seen_gen;
} kvm_vcpu_t;

/* kvm.c */
void kvm_init(void);                   /* /dev/kvm when the CPU can run guests */

/* vmx.c */
int  vmx_probe(void);                  /* 0: VT-x with EPT and unrestricted guest */
int  vmx_vcpu_init(kvm_vcpu_t *v);     /* VMCS with the reset state */
void vmx_vcpu_free(kvm_vcpu_t *v);
int  vmx_vcpu_run(kvm_vcpu_t *v);      /* KVM_RUN */
int  vmx_get_regs(kvm_vcpu_t *v, struct kvm_regs *r);
int  vmx_set_regs(kvm_vcpu_t *v, const struct kvm_regs *r);
int  vmx_get_sregs(kvm_vcpu_t *v, struct kvm_sregs *s);
int  vmx_set_sregs(kvm_vcpu_t *v, const struct kvm_sregs *s);
void vmx_cpu_reset(int cpu);           /* the CPU came back from S3: VMX is off */
int  vmx_enter(vmx_gregs_t *g, int launched);   /* vmx_entry.S: 0 after an exit, 1 VMfail */
void vmx_exit_entry(void);

/* kvm_cpuid.c */
void kvm_cpuid_init(void);
int  kvm_cpuid_supported(struct kvm_cpuid2 *ucpuid);
int  kvm_cpuid_set(kvm_vcpu_t *v, struct kvm_cpuid2 *ucpuid);
int  kvm_cpuid_get(kvm_vcpu_t *v, struct kvm_cpuid2 *ucpuid);
void kvm_cpuid_guest(kvm_vcpu_t *v, uint32_t fn, uint32_t idx, uint32_t r[4]);

/* kvm_msr.c (VMCS loaded) */
void kvm_msr_reset(kvm_vcpu_t *v);
void kvm_msr_host_save(kvm_vcpu_t *v);
int  kvm_msr_get(kvm_vcpu_t *v, uint32_t idx, uint64_t *val);
int  kvm_msr_set(kvm_vcpu_t *v, uint32_t idx, uint64_t val);
int  kvm_msr_index_list(struct kvm_msr_list *ul);

/* kvm_emul.c (VMCS loaded) */
#define KVM_MMIO_USER  0   /* kvm_run says what to do */
#define KVM_MMIO_GUEST 1   /* done here (an ioeventfd): enter the guest again */
#define KVM_MMIO_RETRY 2   /* v->prefault must be mapped first */
int  kvm_mmio_exit(kvm_vcpu_t *v, uint64_t gpa);
void kvm_mmio_finish(kvm_vcpu_t *v);

/* vmx.c internals that vmx_state.c shares */
int  vmcs_load(kvm_vcpu_t *v, uint64_t *irqf);   /* the CPU, or <0; interrupts off until vmcs_put */
void vmcs_put(kvm_vcpu_t *v, uint64_t irqf);
void vmx_seg_set(int i, const struct kvm_segment *s);
void vmx_seg_get(int i, struct kvm_segment *s);
void vmx_cr0_apply(kvm_vcpu_t *v);
void vmx_cr4_apply(kvm_vcpu_t *v);
int  vmx_pdptrs_load(kvm_vcpu_t *v);

/* vmx_state.c and the vmx.c ones the ioctls use */
void vmx_efer_update(kvm_vcpu_t *v);
int  vmx_get_msrs(kvm_vcpu_t *v, struct kvm_msrs *u, int set);
int  vmx_get_events(kvm_vcpu_t *v, struct kvm_vcpu_events *e);
int  vmx_set_events(kvm_vcpu_t *v, const struct kvm_vcpu_events *e);
int  vmx_get_debugregs(kvm_vcpu_t *v, struct kvm_debugregs *d);
int  vmx_set_debugregs(kvm_vcpu_t *v, const struct kvm_debugregs *d);
int  vmx_set_xsave(kvm_vcpu_t *v, const uint8_t *area);
int  vmx_set_xcr0(kvm_vcpu_t *v, uint64_t val);

/* kvm_ioev.c */
int  kvm_ioeventfd(kvm_vm_t *vm, const struct kvm_ioeventfd *a);
int  kvm_ioeventfd_hit(kvm_vm_t *vm, int pio, uint64_t addr, uint32_t len, uint64_t data);
void kvm_ioeventfd_free(kvm_vm_t *vm);

/* kvm_ept.c */
void kvm_vm_quiesce(kvm_vm_t *vm);   /* no vcpu uses what was published before (vm->lock held) */
int  kvm_ept_init(kvm_vm_t *vm);
void kvm_ept_free(kvm_vm_t *vm);
int  kvm_ept_fault(kvm_vm_t *vm, uint64_t gpa);   /* 1 mapped, 0 no slot there, <0 error */
void kvm_ept_unmap(kvm_vm_t *vm, uint64_t gpa, uint64_t size);
/* -EAGAIN: *missing is a page not mapped yet */
int  kvm_gpa_read(kvm_vm_t *vm, uint64_t gpa, void *buf, size_t n, uint64_t *missing);
kvm_slot_t *kvm_slot_find(kvm_vm_t *vm, uint64_t gpa);
int  kvm_slot_present(kvm_vm_t *vm, uint64_t gpa);  /* without the lock: 1, 0, or -1 changing */

#endif
