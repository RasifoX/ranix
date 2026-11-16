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

typedef struct kvm_vm {
	mutex_t     lock;        /* slots, vcpus[], every change to the EPT tables */
	vm_space_t *vs;          /* the creator's address space: slots name its addresses */
	uint32_t    refs;        /* the VM fd and each vcpu */
	uint64_t    ept_root;    /* physical PML4 of the EPT tables */
	uint64_t    eptp;
	kvm_slot_t  slots[KVM_MAX_SLOTS];
	struct kvm_vcpu *vcpus[KVM_MAX_VCPUS];
	/* EPT translations a CPU may still cache. ept_gen moves when a mapping is
	 * taken away; a vcpu entering on a CPU whose cpu_gen is behind flushes it
	 * first (see kvm_ept_quiesce). */
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
	/* kvm_ept_quiesce: the CPU running this vcpu with the VMCS loaded (-1 none)
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

/* kvm_ept.c */
int  kvm_ept_init(kvm_vm_t *vm);
void kvm_ept_free(kvm_vm_t *vm);
int  kvm_ept_fault(kvm_vm_t *vm, uint64_t gpa);   /* 1 mapped, 0 no slot there, <0 error */
void kvm_ept_unmap(kvm_vm_t *vm, uint64_t gpa, uint64_t size);
/* -EAGAIN: *missing is a page not mapped yet */
int  kvm_gpa_read(kvm_vm_t *vm, uint64_t gpa, void *buf, size_t n, uint64_t *missing);
kvm_slot_t *kvm_slot_find(kvm_vm_t *vm, uint64_t gpa);

#endif
