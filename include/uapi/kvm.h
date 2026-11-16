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
#define KVM_EXIT_INTERNAL_ERROR   17

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
#define KVM_CHECK_EXTENSION        KVM_IO(0x03)
#define KVM_GET_VCPU_MMAP_SIZE     KVM_IO(0x04)

/* ioctls on a VM fd */
#define KVM_CREATE_VCPU            KVM_IO(0x41)   /* arg: vcpu id */
#define KVM_SET_USER_MEMORY_REGION KVM_IOW(0x46, struct kvm_userspace_memory_region)

/* ioctls on a vcpu fd */
#define KVM_RUN                    KVM_IO(0x80)
#define KVM_GET_REGS               KVM_IOR(0x81, struct kvm_regs)
#define KVM_SET_REGS               KVM_IOW(0x82, struct kvm_regs)
#define KVM_GET_SREGS              KVM_IOR(0x83, struct kvm_sregs)
#define KVM_SET_SREGS              KVM_IOW(0x84, struct kvm_sregs)

/* KVM_CHECK_EXTENSION */
#define KVM_CAP_USER_MEMORY 3
#define KVM_CAP_NR_VCPUS    9
#define KVM_CAP_NR_MEMSLOTS 10
#define KVM_CAP_MAX_VCPUS   66
#define KVM_CAP_IMMEDIATE_EXIT 136

#endif
