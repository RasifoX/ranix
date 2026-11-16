/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* /dev/kvm (arch/x86_64/kvm.c, vmx.c): a real-mode guest through the KVM ioctls.
 *
 * The guest is 16-bit code at guest-physical 0x1000. It writes a string to
 * port 0x3f8 a byte at a time, reads a byte from port 0x40 (this program
 * answers 'K'), stores it plus one at 0x5000 and sends it to port 0x41, then
 * halts. That covers VM and vcpu creation, a memory slot reached through EPT
 * (code page, data page), OUT and IN exits through the mmapped kvm_run, and
 * HLT. Then the refusals: the device's mode and group, a user outside the kvm
 * group, a second VM type, slot and vcpu errors, an unknown ioctl, a vcpu run
 * from another address space, immediate_exit.
 *
 * A second guest covers what a VMM such as QEMU needs: CPUID from the table
 * KVM_SET_CPUID2 gave, RDMSR/WRMSR against KVM_GET/SET_MSRS, a store and a load
 * to device memory (KVM_EXIT_MMIO, both decoded forms), a port write an
 * ioeventfd takes without an exit, REP OUTS forwards and backwards and REP INS,
 * and an interrupt injected with
 * KVM_INTERRUPT after HLT, served by the guest's real-mode handler. Also
 * KVM_GET_SUPPORTED_CPUID, the MSR index list, the FPU, XCR0 and vcpu
 * events round trips.
 *
 * A third VM has its local APICs in the kernel (KVM_CAP_SPLIT_IRQCHIP), moved
 * to 0xF0000 so real-mode code reaches them: the APIC timer wakes a HLT, an MSI
 * from KVM_SIGNAL_MSI and one from an irqfd are taken, an INIT level de-assert
 * sent to everyone leaves the sender running, and INIT then SIPI start the
 * second vcpu (waiting in KVM_RUN on its own thread) at the SIPI's page.
 *
 * Prints OK, or SKIP where the CPU has no VMX (or is not x86), else FAIL lines. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/eventfd.h>
#include <pthread.h>
#include <grp.h>
#include <stdlib.h>
#include "../include/uapi/kvm.h"

static int fails;
#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } while (0)

#if defined(__x86_64__)
#include <cpuid.h>

extern const unsigned char guest16_start[], guest16_hlt_end[], guest16_end[];
__asm__(
	".pushsection .rodata\n"
	".globl guest16_start, guest16_hlt_end, guest16_end\n"
	"guest16_start:\n"
	".code16\n"
	"	mov $0x3f8, %dx\n"
	"	mov $(0x1000 + guest16_msg - guest16_start), %si\n"
	"1:	lodsb\n"
	"	test %al, %al\n"
	"	jz 2f\n"
	"	out %al, %dx\n"
	"	jmp 1b\n"
	"2:	in $0x40, %al\n"
	"	inc %al\n"
	"	movb %al, 0x5000\n"
	"	out %al, $0x41\n"
	"	hlt\n"
	"guest16_hlt_end:\n"
	"guest16_msg: .asciz \"Ranix KVM\\n\"\n"
	".code64\n"
	"guest16_end:\n"
	".popsection\n");

/* The second guest, also at 0x1000, in 16-bit code with 32-bit registers. */
extern const unsigned char guest2_start[], guest2_end[];
__asm__(
	".pushsection .rodata\n"
	".globl guest2_start, guest2_end\n"
	"guest2_start:\n"
	".code16\n"
	"	xor %eax, %eax\n"
	"	cpuid\n"                         /* leaf 0 from the vcpu's table */
	"	mov %bl, %al\n"
	"	out %al, $0x10\n"
	"	mov $0xC0000081, %ecx\n"         /* STAR, set by userspace */
	"	rdmsr\n"
	"	mov %dl, %al\n"                  /* STAR bits 39:32 */
	"	out %al, $0x11\n"
	"	mov $0xC0000082, %ecx\n"         /* LSTAR, read back by userspace */
	"	mov $0x66554433, %eax\n"
	"	mov $0xFFFF8877, %edx\n"         /* canonical, or WRMSR would #GP */
	"	wrmsr\n"
	"	mov $0x2000, %ax\n"              /* DS at 0x20000: no memory slot there */
	"	mov %ax, %ds\n"
	"	movb $0x5A, 0x10\n"              /* MMIO store, immediate */
	"	mov 0x20, %bx\n"                 /* MMIO load, 16 bits */
	"	xor %ax, %ax\n"
	"	mov %ax, %ds\n"
	"	mov %bx, %ax\n"
	"	out %ax, $0x12\n"
	"	cld\n"                           /* string port I/O, one exit each */
	"	mov $0x16, %dx\n"
	"	mov $(0x1000 + g2_msg - guest2_start), %si\n"
	"	mov $5, %cx\n"
	"	rep outsb\n"                     /* \"Ranix\", forwards */
	"	std\n"
	"	mov $(0x1000 + g2_msg + 7 - guest2_start), %si\n"
	"	mov $3, %cx\n"
	"	rep outsb\n"                     /* \"KVM\" from its end: M, V, K */
	"	cld\n"
	"	mov $0x17, %dx\n"
	"	mov $0x3000, %di\n"
	"	mov $4, %cx\n"
	"	rep insb\n"                      /* four bytes from userspace to 0x3000 */
	"	out %al, $0x50\n"                /* the ioeventfd's port */
	"	sti\n"
	"	hlt\n"                           /* userspace injects vector 0x20 here */
	"	cli\n"
	"	hlt\n"
	"g2_msg: .ascii \"RanixKVM\"\n"
	".org guest2_start + 0x800\n"        /* 0x1800: the handler of vector 0x20 */
	"	mov $0x77, %al\n"
	"	out %al, $0x14\n"
	"	iret\n"
	".code64\n"
	"guest2_end:\n"
	".popsection\n");

/* The APIC guest: vcpu 0 at 0x1000 with DS on the APIC page (0xF0000), vcpu 1
 * started by SIPI vector 2 at 0x2000. Handlers at 0x1800 (timer, vector
 * 0x30), 0x1820 (MSI, 0x31), 0x1840 (irqfd, 0x35). */
extern const unsigned char guest3_start[], guest3_end[];
__asm__(
	".pushsection .rodata\n"
	".globl guest3_start, guest3_end\n"
	"guest3_start:\n"
	".code16\n"
	"	mov $0xF000, %ax\n"
	"	mov %ax, %ds\n"
	"	movl $0x1FF, 0xF0\n"             /* SVR: on, spurious vector 0xFF */
	"	movl $0xB, 0x3E0\n"              /* divide by 1 */
	"	movl $0x30, 0x320\n"             /* timer: vector 0x30, one-shot */
	"	movl $200000, 0x380\n"           /* 200 us */
	"	sti\n"                           /* STI; HLT: nothing is taken between them */
	"	hlt\n"                           /* the timer ends it */
	"	cli\n"
	"	out %al, $0x31\n"                /* userspace sends an MSI here */
	"	sti\n"
	"	hlt\n"
	"	cli\n"
	"	out %al, $0x34\n"                /* userspace signals the irqfd here */
	"	sti\n"
	"	hlt\n"
	"	movl $0x88500, 0x300\n"          /* INIT level de-assert to all, self too */
	"	out %al, $0x36\n"                /* still here: it did not reset us */
	"	movl $0x01000000, 0x310\n"       /* APIC ID 1 */
	"	movl $0x4500, 0x300\n"           /* INIT */
	"	movl $0x4602, 0x300\n"           /* SIPI, vector 2: 0x2000 */
	"	out %al, $0x37\n"
	".org guest3_start + 0x800\n"
	"	mov $0x30, %al\n"
	"	out %al, $0x30\n"
	"	movl $0, 0xB0\n"                 /* EOI */
	"	iret\n"
	".org guest3_start + 0x820\n"
	"	mov $0x33, %al\n"
	"	out %al, $0x33\n"
	"	movl $0, 0xB0\n"
	"	iret\n"
	".org guest3_start + 0x840\n"
	"	mov $0x35, %al\n"
	"	out %al, $0x35\n"
	"	movl $0, 0xB0\n"
	"	iret\n"
	".org guest3_start + 0x1000\n"       /* 0x2000: vcpu 1 */
	"	mov $0x41, %al\n"
	"	out %al, $0x40\n"
	"	cli\n"
	"	hlt\n"
	".code64\n"
	"guest3_end:\n"
	".popsection\n");

#define MEM_SIZE 0x10000

static int has_vmx(void)
{
	unsigned a, b, c, d;
	if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
	return (c >> 5) & 1;
}

/* the device node: 0660 root:kvm (34), and a user outside the group is refused */
static void check_node(void)
{
	struct stat st;
	if (stat("/dev/kvm", &st) != 0) { FAIL("stat /dev/kvm: %s", strerror(errno)); return; }
	if ((st.st_mode & 07777) != 0660 || st.st_uid != 0 || st.st_gid != 34)
		FAIL("/dev/kvm mode %o uid %d gid %d", st.st_mode & 07777, (int)st.st_uid, (int)st.st_gid);
	pid_t pid = fork();
	if (pid == 0) {
		gid_t none[1] = { 1000 };
		if (setgroups(1, none) || setgid(1000) || setuid(1000)) _exit(2);
		int fd = open("/dev/kvm", O_RDWR);
		_exit(fd < 0 && errno == EACCES ? 0 : 1);
	}
	int st2 = 0;
	waitpid(pid, &st2, 0);
	if (!WIFEXITED(st2) || WEXITSTATUS(st2) != 0) FAIL("uid 1000 opened /dev/kvm (status %d)", st2);
}

static int set_slot(int vm, uint32_t slot, uint64_t gpa, uint64_t size, void *hva)
{
	struct kvm_userspace_memory_region r = {
		.slot = slot, .guest_phys_addr = gpa, .memory_size = size,
		.userspace_addr = (uint64_t)(uintptr_t)hva,
	};
	return ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r);
}

static void expect_errno(const char *what, int r, int err)
{
	if (r != -1 || errno != err) FAIL("%s: got %d errno %d, want errno %d", what, r, errno, err);
}

/* KVM_RUN until HLT, playing the two ports */
static void run_guest(int vcpu, struct kvm_run *run, char *out, size_t cap, int *port41)
{
	size_t n = 0;
	for (int i = 0; i < 1000; i++) {
		if (ioctl(vcpu, KVM_RUN, 0) != 0) { FAIL("KVM_RUN: %s", strerror(errno)); return; }
		uint8_t *data = (uint8_t *)run + run->io.data_offset;
		switch (run->exit_reason) {
		case KVM_EXIT_IO:
			if (run->io.size != 1 || run->io.count != 1) {
				FAIL("io size %d count %u", run->io.size, run->io.count);
				return;
			}
			if (run->io.direction == KVM_EXIT_IO_OUT && run->io.port == 0x3f8) {
				if (n + 1 < cap) out[n++] = (char)data[0];
			} else if (run->io.direction == KVM_EXIT_IO_IN && run->io.port == 0x40) {
				data[0] = 'K';
			} else if (run->io.direction == KVM_EXIT_IO_OUT && run->io.port == 0x41) {
				*port41 = data[0];
			} else {
				FAIL("io dir %d port 0x%x", run->io.direction, run->io.port);
				return;
			}
			break;
		case KVM_EXIT_HLT:
			out[n] = 0;
			return;
		default:
			FAIL("exit reason %u (hw 0x%llx, internal %u %llx %llx)", run->exit_reason,
			     (unsigned long long)run->fail_entry.hardware_entry_failure_reason,
			     run->internal.suberror, (unsigned long long)run->internal.data[0],
			     (unsigned long long)run->internal.data[1]);
			return;
		}
	}
	FAIL("no HLT after 1000 exits");
}

static void test_guest(int kvm)
{
	expect_errno("KVM_CREATE_VM type 1", ioctl(kvm, KVM_CREATE_VM, 1), EINVAL);
	int vm = ioctl(kvm, KVM_CREATE_VM, 0);
	if (vm < 0) { FAIL("KVM_CREATE_VM: %s", strerror(errno)); return; }
	if (!(fcntl(vm, F_GETFD) & FD_CLOEXEC)) FAIL("VM fd without close-on-exec");
	if (ioctl(vm, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY) != 1) FAIL("KVM_CAP_USER_MEMORY");

	uint8_t *mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (mem == MAP_FAILED) { FAIL("mmap guest memory"); return; }
	memcpy(mem + 0x1000, guest16_start, (size_t)(guest16_end - guest16_start));
	if (set_slot(vm, 0, 0, MEM_SIZE, mem) != 0) FAIL("slot 0: %s", strerror(errno));
	expect_errno("overlapping slot", set_slot(vm, 1, 0x8000, 0x1000, mem), EEXIST);
	expect_errno("unaligned slot", set_slot(vm, 1, 0x20000, 0x800, mem), EINVAL);
	expect_errno("slot number past the end", set_slot(vm, 4096, 0x20000, 0x1000, mem), EINVAL);

	int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
	if (vcpu < 0) { FAIL("KVM_CREATE_VCPU: %s", strerror(errno)); return; }
	expect_errno("second vcpu 0", ioctl(vm, KVM_CREATE_VCPU, 0), EEXIST);
	expect_errno("unknown vcpu ioctl", ioctl(vcpu, KVM_IO(0xFF), 0), ENOTTY);
	int msz = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (msz < (int)sizeof(struct kvm_run)) { FAIL("mmap size %d", msz); return; }
	struct kvm_run *run = mmap(NULL, (size_t)msz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
	if (run == MAP_FAILED) { FAIL("mmap kvm_run: %s", strerror(errno)); return; }

	struct kvm_sregs sregs;
	if (ioctl(vcpu, KVM_GET_SREGS, &sregs) != 0) { FAIL("KVM_GET_SREGS: %s", strerror(errno)); return; }
	if (sregs.cs.selector != 0xF000 || sregs.cs.base != 0xFFFF0000 || sregs.cr0 != 0x60000010)
		FAIL("reset state cs %x base %llx cr0 %llx", sregs.cs.selector,
		     (unsigned long long)sregs.cs.base, (unsigned long long)sregs.cr0);
	sregs.cs.selector = 0;
	sregs.cs.base = 0;
	if (ioctl(vcpu, KVM_SET_SREGS, &sregs) != 0) FAIL("KVM_SET_SREGS: %s", strerror(errno));
	struct kvm_regs regs;
	memset(&regs, 0, sizeof(regs));
	regs.rip = 0x1000;
	regs.rflags = 0x2;
	if (ioctl(vcpu, KVM_SET_REGS, &regs) != 0) FAIL("KVM_SET_REGS: %s", strerror(errno));

	run->immediate_exit = 1;
	expect_errno("immediate_exit", ioctl(vcpu, KVM_RUN, 0), EINTR);
	run->immediate_exit = 0;

	/* another address space may not run it */
	pid_t pid = fork();
	if (pid == 0) _exit(ioctl(vcpu, KVM_RUN, 0) == -1 && errno == EIO ? 0 : 1);
	int st = 0;
	waitpid(pid, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) FAIL("KVM_RUN from a forked child was not EIO");

	char out[64];
	int port41 = -1;
	run_guest(vcpu, run, out, sizeof(out), &port41);
	if (strcmp(out, "Ranix KVM\n") != 0) FAIL("guest wrote '%s'", out);
	if (port41 != 'L') FAIL("port 0x41 got %d", port41);
	if (mem[0x5000] != 'L') FAIL("guest memory at 0x5000 is %d", mem[0x5000]);
	if (ioctl(vcpu, KVM_GET_REGS, &regs) != 0) FAIL("KVM_GET_REGS: %s", strerror(errno));
	uint64_t want_rip = 0x1000 + (uint64_t)(guest16_hlt_end - guest16_start);
	if (regs.rip != want_rip) FAIL("rip after HLT %llx, want %llx",
	                               (unsigned long long)regs.rip, (unsigned long long)want_rip);
	if ((regs.rax & 0xFF) != 'L') FAIL("al after the guest %llx", (unsigned long long)regs.rax);

	munmap(run, (size_t)msz);
	close(vcpu);
	close(vm);
	munmap(mem, MEM_SIZE);
}

/* KVM_GET_SUPPORTED_CPUID, the MSR index list: what a VMM reads first */
static void test_tables(int kvm)
{
	struct { struct kvm_cpuid2 h; struct kvm_cpuid_entry2 e[100]; } c;
	c.h.nent = 1;
	expect_errno("supported CPUID into one entry", ioctl(kvm, KVM_GET_SUPPORTED_CPUID, &c), E2BIG);
	c.h.nent = 100;
	if (ioctl(kvm, KVM_GET_SUPPORTED_CPUID, &c) != 0) { FAIL("KVM_GET_SUPPORTED_CPUID: %s", strerror(errno)); return; }
	int leaf1 = 0;
	for (unsigned i = 0; i < c.h.nent; i++) {
		if (c.e[i].function != 1) continue;
		leaf1 = 1;
		if (c.e[i].ecx & (1U << 5)) FAIL("supported CPUID offers VMX");
		if (!(c.e[i].ecx & (1U << 31))) FAIL("supported CPUID lacks the hypervisor bit");
	}
	if (!leaf1) FAIL("supported CPUID has no leaf 1");
	struct { struct kvm_msr_list h; uint32_t idx[64]; } l;
	l.h.nmsrs = 64;
	if (ioctl(kvm, KVM_GET_MSR_INDEX_LIST, &l) != 0) { FAIL("KVM_GET_MSR_INDEX_LIST: %s", strerror(errno)); return; }
	int lstar = 0;
	for (unsigned i = 0; i < l.h.nmsrs; i++) lstar |= l.idx[i] == 0xC0000082;
	if (!lstar) FAIL("MSR index list without LSTAR");
}

static int msr_io(int vcpu, unsigned long req, uint32_t idx, uint64_t *val)
{
	struct { struct kvm_msrs h; struct kvm_msr_entry e; } m;
	memset(&m, 0, sizeof(m));
	m.h.nmsrs = 1;
	m.e.index = idx;
	m.e.data = *val;
	int r = ioctl(vcpu, req, &m);
	*val = m.e.data;
	return r;
}

/* the second guest: one stop at a time, each checked */
static void run_guest2(int vcpu, struct kvm_run *run, int efd)
{
	struct { unsigned port, size, val; } want[] = {
		{ 0x10, 1, 'R' }, { 0x11, 1, 0x44 }, { 0, 0, 0 } /* MMIO store */, { 1, 0, 0 } /* MMIO load */,
		{ 0x12, 2, 0x1234 }, { 4, 0, 0 } /* OUTS */, { 5, 0, 0 } /* OUTS, DF set */, { 6, 0, 0 } /* INS */,
		{ 2, 0, 0 } /* HLT, inject */, { 0x14, 1, 0x77 }, { 3, 0, 0 } /* HLT */,
	};
	for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
		if (ioctl(vcpu, KVM_RUN, 0) != 0) { FAIL("guest2 KVM_RUN %u: %s", i, strerror(errno)); return; }
		if (want[i].size) {
			if (run->exit_reason != KVM_EXIT_IO) {
				FAIL("guest2 stop %u: exit %u, want port 0x%x (internal %u %llx %llx)", i, run->exit_reason,
				     want[i].port, run->internal.suberror, (unsigned long long)run->internal.data[0],
				     (unsigned long long)run->internal.data[1]);
				return;
			}
			uint8_t *data = (uint8_t *)run + run->io.data_offset;
			unsigned v = run->io.size == 2 ? (unsigned)(data[0] | data[1] << 8) : data[0];
			if (run->io.port != want[i].port || run->io.size != want[i].size || v != want[i].val) {
				FAIL("guest2 stop %u: exit %u port 0x%x size %u value 0x%x", i, run->exit_reason,
				     run->io.port, run->io.size, v);
				return;
			}
			continue;
		}
		switch (want[i].port) {
		case 0:
			if (run->exit_reason != KVM_EXIT_MMIO || !run->mmio.is_write || run->mmio.phys_addr != 0x20010 ||
			    run->mmio.len != 1 || run->mmio.data[0] != 0x5A) {
				FAIL("guest2 MMIO store: exit %u w%u addr 0x%llx len %u data 0x%x (internal %u %llx %llx)",
				     run->exit_reason, run->mmio.is_write, (unsigned long long)run->mmio.phys_addr,
				     run->mmio.len, run->mmio.data[0], run->internal.suberror,
				     (unsigned long long)run->internal.data[0], (unsigned long long)run->internal.data[1]);
				return;
			}
			break;
		case 1:
			if (run->exit_reason != KVM_EXIT_MMIO || run->mmio.is_write || run->mmio.phys_addr != 0x20020 ||
			    run->mmio.len != 2) {
				FAIL("guest2 MMIO load: exit %u w%u addr 0x%llx len %u", run->exit_reason,
				     run->mmio.is_write, (unsigned long long)run->mmio.phys_addr, run->mmio.len);
				return;
			}
			run->mmio.data[0] = 0x34;
			run->mmio.data[1] = 0x12;
			break;
		case 2: {
			if (run->exit_reason != KVM_EXIT_HLT) { FAIL("guest2: exit %u, want HLT", run->exit_reason); return; }
			uint64_t n = 0;
			if (read(efd, &n, sizeof(n)) != sizeof(n) || n != 1) FAIL("ioeventfd count %llu", (unsigned long long)n);
			if (!run->if_flag || !run->ready_for_interrupt_injection)
				FAIL("after STI; HLT: if_flag %u ready %u", run->if_flag, run->ready_for_interrupt_injection);
			struct kvm_interrupt irq = { 0x20 };
			if (ioctl(vcpu, KVM_INTERRUPT, &irq) != 0) FAIL("KVM_INTERRUPT: %s", strerror(errno));
			expect_errno("a second KVM_INTERRUPT", ioctl(vcpu, KVM_INTERRUPT, &irq), EEXIST);
			break;
		}
		case 3:
			if (run->exit_reason != KVM_EXIT_HLT) FAIL("guest2 end: exit %u", run->exit_reason);
			break;
		case 4: case 5: case 6: {
			static const struct { unsigned port, dir, count; const char *data; } str[] = {
				{ 0x16, KVM_EXIT_IO_OUT, 5, "Ranix" }, { 0x16, KVM_EXIT_IO_OUT, 3, "MVK" },
				{ 0x17, KVM_EXIT_IO_IN, 4, "ABCD" },
			};
			unsigned k = want[i].port - 4;
			uint8_t *data = (uint8_t *)run + run->io.data_offset;
			if (run->exit_reason != KVM_EXIT_IO || run->io.port != str[k].port || run->io.direction != str[k].dir ||
			    run->io.size != 1 || run->io.count != str[k].count) {
				FAIL("guest2 string I/O %u: exit %u port 0x%x dir %u size %u count %u (internal %u %llx)", k,
				     run->exit_reason, run->io.port, run->io.direction, run->io.size, run->io.count,
				     run->internal.suberror, (unsigned long long)run->internal.data[1]);
				return;
			}
			if (str[k].dir == KVM_EXIT_IO_IN) memcpy(data, str[k].data, str[k].count);
			else if (memcmp(data, str[k].data, str[k].count)) FAIL("OUTS %u sent '%.*s'", k, (int)str[k].count, data);
			break;
		}
		}
	}
}

static void test_guest2(int kvm)
{
	int vm = ioctl(kvm, KVM_CREATE_VM, 0);
	uint8_t *mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (vm < 0 || mem == MAP_FAILED) { FAIL("guest2 VM"); return; }
	memcpy(mem + 0x1000, guest2_start, (size_t)(guest2_end - guest2_start));
	mem[0x80] = 0x00; mem[0x81] = 0x18; mem[0x82] = 0; mem[0x83] = 0;   /* IVT[0x20] = 0000:1800 */
	if (set_slot(vm, 0, 0, MEM_SIZE, mem) != 0) FAIL("guest2 slot: %s", strerror(errno));
	int efd = eventfd(0, EFD_NONBLOCK);
	struct kvm_ioeventfd io = { .addr = 0x50, .len = 1, .fd = efd, .flags = KVM_IOEVENTFD_FLAG_PIO };
	if (ioctl(vm, KVM_IOEVENTFD, &io) != 0) FAIL("KVM_IOEVENTFD: %s", strerror(errno));
	expect_errno("the same ioeventfd twice", ioctl(vm, KVM_IOEVENTFD, &io), EEXIST);
	struct kvm_ioeventfd bad = io;
	bad.fd = vm;
	bad.addr = 0x51;
	expect_errno("an ioeventfd that is no eventfd", ioctl(vm, KVM_IOEVENTFD, &bad), EINVAL);

	int vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
	size_t msz = (size_t)ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);   /* port data is past kvm_run */
	struct kvm_run *run = mmap(NULL, msz, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
	if (vcpu < 0 || run == MAP_FAILED) { FAIL("guest2 vcpu"); return; }
	struct { struct kvm_cpuid2 h; struct kvm_cpuid_entry2 e[1]; } c;
	memset(&c, 0, sizeof(c));
	c.h.nent = 1;
	c.e[0].function = 0;
	c.e[0].eax = 1;
	c.e[0].ebx = 'R';
	if (ioctl(vcpu, KVM_SET_CPUID2, &c) != 0) FAIL("KVM_SET_CPUID2: %s", strerror(errno));
	uint64_t star = 0x1122334455667788ULL, junk = 1;
	if (msr_io(vcpu, KVM_SET_MSRS, 0xC0000081, &star) != 1) FAIL("KVM_SET_MSRS STAR");
	if (msr_io(vcpu, KVM_SET_MSRS, 0x12345, &junk) != 0) FAIL("KVM_SET_MSRS took an MSR that is not there");

	struct kvm_sregs sregs;
	ioctl(vcpu, KVM_GET_SREGS, &sregs);
	sregs.cs.selector = 0;
	sregs.cs.base = 0;
	ioctl(vcpu, KVM_SET_SREGS, &sregs);
	struct kvm_regs regs;
	memset(&regs, 0, sizeof(regs));
	regs.rip = 0x1000;
	regs.rflags = 0x2;
	regs.rsp = 0x8000;
	ioctl(vcpu, KVM_SET_REGS, &regs);

	run_guest2(vcpu, run, efd);
	if (memcmp(mem + 0x3000, "ABCD", 4)) FAIL("INS stored '%.4s'", mem + 0x3000);
	uint64_t lstar = 0;
	if (msr_io(vcpu, KVM_GET_MSRS, 0xC0000082, &lstar) != 1 || lstar != 0xFFFF887766554433ULL)
		FAIL("LSTAR the guest wrote reads back as %llx", (unsigned long long)lstar);

	struct kvm_fpu fpu;
	if (ioctl(vcpu, KVM_GET_FPU, &fpu) != 0) FAIL("KVM_GET_FPU: %s", strerror(errno));
	if (fpu.fcw != 0x37F) FAIL("FCW %x", fpu.fcw);
	fpu.mxcsr = 0x1F80 | 0x6000;   /* round toward zero */
	ioctl(vcpu, KVM_SET_FPU, &fpu);
	memset(&fpu, 0, sizeof(fpu));
	ioctl(vcpu, KVM_GET_FPU, &fpu);
	if (fpu.mxcsr != (0x1F80 | 0x6000)) FAIL("MXCSR after KVM_SET_FPU %x", fpu.mxcsr);
	fpu.mxcsr = 0x10000;
	expect_errno("a reserved MXCSR bit", ioctl(vcpu, KVM_SET_FPU, &fpu), EINVAL);

	struct kvm_xcrs x;
	memset(&x, 0, sizeof(x));
	x.nr_xcrs = 1;
	x.xcrs[0].value = 2;   /* SSE without x87 */
	expect_errno("XCR0 without x87", ioctl(vcpu, KVM_SET_XCRS, &x), EINVAL);

	struct kvm_vcpu_events ev;
	if (ioctl(vcpu, KVM_GET_VCPU_EVENTS, &ev) != 0) FAIL("KVM_GET_VCPU_EVENTS: %s", strerror(errno));
	ev.nmi.masked = 1;
	ev.flags = 0;
	if (ioctl(vcpu, KVM_SET_VCPU_EVENTS, &ev) != 0) FAIL("KVM_SET_VCPU_EVENTS: %s", strerror(errno));
	memset(&ev, 0, sizeof(ev));
	ioctl(vcpu, KVM_GET_VCPU_EVENTS, &ev);
	if (!ev.nmi.masked) FAIL("NMI mask did not stick");

	munmap(run, msz);
	close(vcpu);
	close(efd);
	close(vm);
	munmap(mem, MEM_SIZE);
}

struct vcpu1 { int fd; struct kvm_run *run; int got; };

/* vcpu 1 waits in KVM_RUN for its SIPI, then stops at its first port write */
static void *vcpu1_thread(void *arg)
{
	struct vcpu1 *c = arg;
	if (ioctl(c->fd, KVM_RUN, 0) == 0 && c->run->exit_reason == KVM_EXIT_IO && c->run->io.port == 0x40)
		c->got = ((uint8_t *)c->run + c->run->io.data_offset)[0];
	return NULL;
}

static int run_until_port(int vcpu, struct kvm_run *run, unsigned port)
{
	if (ioctl(vcpu, KVM_RUN, 0) != 0) { FAIL("apic KVM_RUN: %s", strerror(errno)); return -1; }
	if (run->exit_reason != KVM_EXIT_IO || run->io.port != port) {
		FAIL("apic guest: exit %u port 0x%x, want port 0x%x (internal %u %llx %llx)", run->exit_reason,
		     run->io.port, port, run->internal.suberror, (unsigned long long)run->internal.data[0],
		     (unsigned long long)run->internal.data[1]);
		return -1;
	}
	return 0;
}

static void test_apic(int kvm)
{
	int vm = ioctl(kvm, KVM_CREATE_VM, 0);
	uint8_t *mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (vm < 0 || mem == MAP_FAILED) { FAIL("apic VM"); return; }
	struct kvm_irq_level line = { .irq = 5, .level = 1 };
	expect_errno("KVM_IRQ_LINE without an irqchip", ioctl(vm, KVM_IRQ_LINE, &line), ENXIO);
	struct kvm_enable_cap cap = { .cap = KVM_CAP_SPLIT_IRQCHIP, .args = { 24 } };
	if (ioctl(vm, KVM_ENABLE_CAP, &cap) != 0) { FAIL("KVM_CAP_SPLIT_IRQCHIP: %s", strerror(errno)); return; }
	memcpy(mem + 0x1000, guest3_start, (size_t)(guest3_end - guest3_start));
	const struct { int vec; uint16_t off; } ivt[] = { { 0x30, 0x1800 }, { 0x31, 0x1820 }, { 0x35, 0x1840 } };
	for (int i = 0; i < 3; i++) {
		mem[ivt[i].vec * 4] = (uint8_t)ivt[i].off;
		mem[ivt[i].vec * 4 + 1] = (uint8_t)(ivt[i].off >> 8);
	}
	if (set_slot(vm, 0, 0, MEM_SIZE, mem) != 0) FAIL("apic slot");
	int v0 = ioctl(vm, KVM_CREATE_VCPU, 0), v1 = ioctl(vm, KVM_CREATE_VCPU, 1);
	if (v0 < 0 || v1 < 0) { FAIL("apic vcpus: %s", strerror(errno)); return; }
	expect_errno("split irqchip after the vcpus", ioctl(vm, KVM_ENABLE_CAP, &cap), EINVAL);
	size_t msz = (size_t)ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
	struct kvm_run *r0 = mmap(NULL, msz, PROT_READ | PROT_WRITE, MAP_SHARED, v0, 0);
	struct kvm_run *r1 = mmap(NULL, msz, PROT_READ | PROT_WRITE, MAP_SHARED, v1, 0);
	struct kvm_mp_state mp;
	if (ioctl(v1, KVM_GET_MP_STATE, &mp) != 0 || mp.mp_state != KVM_MP_STATE_UNINITIALIZED)
		FAIL("vcpu 1 is not waiting for its SIPI (%u)", mp.mp_state);
	struct kvm_lapic_state *ls = malloc(sizeof(*ls));
	if (ioctl(v1, KVM_GET_LAPIC, ls) != 0 || *(uint32_t *)&ls->regs[0x20] != 1U << 24)
		FAIL("vcpu 1's APIC ID %x", *(uint32_t *)&ls->regs[0x20]);
	free(ls);

	struct kvm_sregs sregs;
	struct kvm_regs regs;
	for (int i = 0; i < 2; i++) {
		int fd = i ? v1 : v0;
		ioctl(fd, KVM_GET_SREGS, &sregs);
		sregs.apic_base = 0xF0000 | (1 << 11) | (i ? 0 : 1 << 8);   /* reachable from real mode */
		if (!i) { sregs.cs.selector = 0; sregs.cs.base = 0; }
		if (ioctl(fd, KVM_SET_SREGS, &sregs) != 0) FAIL("apic KVM_SET_SREGS: %s", strerror(errno));
	}
	memset(&regs, 0, sizeof(regs));
	regs.rip = 0x1000;
	regs.rflags = 0x2;
	regs.rsp = 0x8000;
	ioctl(v0, KVM_SET_REGS, &regs);
	memset(&regs, 0, sizeof(regs));
	regs.rflags = 0x2;
	regs.rsp = 0x9000;
	ioctl(v1, KVM_SET_REGS, &regs);

	struct vcpu1 c1 = { v1, r1, 0 };
	pthread_t t;
	pthread_create(&t, NULL, vcpu1_thread, &c1);

	if (run_until_port(v0, r0, 0x30)) goto out;     /* the timer's handler */
	if (run_until_port(v0, r0, 0x31)) goto out;
	struct kvm_msi msi = { .address_lo = 0xFEE00000, .data = 0x31 };
	if (ioctl(vm, KVM_SIGNAL_MSI, &msi) != 1) FAIL("KVM_SIGNAL_MSI: %s", strerror(errno));
	if (run_until_port(v0, r0, 0x33)) goto out;     /* the MSI's handler */
	if (run_until_port(v0, r0, 0x34)) goto out;
	struct { struct kvm_irq_routing h; struct kvm_irq_routing_entry e[1]; } rt;
	memset(&rt, 0, sizeof(rt));
	rt.h.nr = 1;
	rt.e[0].gsi = 5;
	rt.e[0].type = KVM_IRQ_ROUTING_MSI;
	rt.e[0].u.msi.address_lo = 0xFEE00000;
	rt.e[0].u.msi.data = 0x35;
	if (ioctl(vm, KVM_SET_GSI_ROUTING, &rt) != 0) FAIL("KVM_SET_GSI_ROUTING: %s", strerror(errno));
	struct kvm_irq_level none = { .irq = 9, .level = 1 };
	if (ioctl(vm, KVM_IRQ_LINE_STATUS, &none) != 0 || none.status != 0) FAIL("a GSI without a route was delivered");
	int efd = eventfd(0, EFD_NONBLOCK);
	struct kvm_irqfd ifd = { .fd = (uint32_t)efd, .gsi = 5 };
	if (ioctl(vm, KVM_IRQFD, &ifd) != 0) FAIL("KVM_IRQFD: %s", strerror(errno));
	uint64_t one = 1;
	write(efd, &one, sizeof(one));
	if (run_until_port(v0, r0, 0x35)) goto out;     /* the irqfd's handler */
	uint64_t left = 0;
	if (read(efd, &left, sizeof(left)) == sizeof(left)) FAIL("the irqfd left its count (%llu)", (unsigned long long)left);
	if (run_until_port(v0, r0, 0x36)) goto out;     /* INIT de-assert did not reset vcpu 0 */
	if (run_until_port(v0, r0, 0x37)) goto out;     /* INIT, SIPI sent */
	pthread_join(t, NULL);
	if (c1.got != 0x41) FAIL("vcpu 1 did not start at its SIPI vector (%d)", c1.got);
	ifd.flags = KVM_IRQFD_FLAG_DEASSIGN;
	if (ioctl(vm, KVM_IRQFD, &ifd) != 0) FAIL("KVM_IRQFD deassign: %s", strerror(errno));
	close(efd);
	munmap(r0, msz);
	munmap(r1, msz);
	close(v0);
	close(v1);
	close(vm);
	munmap(mem, MEM_SIZE);
	return;
out:
	FAIL("apic guest stopped early");
	_exit(1);   /* vcpu 1's thread may still wait in KVM_RUN */
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);   /* a hang still shows how far it got */
	if (!has_vmx()) { printf("SKIP\n"); return 0; }
	check_node();
	int kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
	if (kvm < 0) { printf("FAIL: open /dev/kvm: %s\n", strerror(errno)); return 1; }
	int api = ioctl(kvm, KVM_GET_API_VERSION, 0);
	if (api != KVM_API_VERSION) FAIL("KVM_GET_API_VERSION %d", api);
	expect_errno("unknown /dev/kvm ioctl", ioctl(kvm, KVM_IO(0xFE), 0), ENOTTY);
	test_guest(kvm);
	test_tables(kvm);
	test_guest2(kvm);
	test_apic(kvm);
	close(kvm);
	printf(fails ? "FAILED %d\n" : "OK\n", fails);
	return fails != 0;
}
#else
int main(void)
{
	printf("SKIP\n");
	return 0;
}
#endif
