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
#include <grp.h>
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
