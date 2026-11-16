/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* Guest memory-mapped I/O for KVM: a guest access to an address no memory slot
 * covers (a device: the local APIC, the IOAPIC, HPET) exits on an EPT
 * violation. The VMM (userspace) must do the access, so the instruction is
 * decoded here: its length, the access size and direction, and the register or
 * immediate it moves. A store goes out as KVM_EXIT_MMIO with its data; a load
 * goes out without, and its value lands in the register at the next KVM_RUN
 * (kvm_mmio_finish).
 *
 * Only the instructions compilers emit for device registers are decoded: MOV
 * in its register, immediate and accumulator/offset forms, and MOVZX/MOVSX. A
 * string move or a read-modify-write to device memory is refused (the VMM sees
 * an internal error with the instruction bytes), which names what to add.
 *
 * Fetching the instruction needs the guest's own page tables: the four paging
 * modes are walked for translation only (the guest just ran the instruction,
 * so it was allowed to).
 *
 * String port I/O (INS, OUTS, with REP) is here too: it is the other guest
 * instruction whose memory operand the exit does not hand over as data.
 *
 * Provenance: instruction encodings (prefixes, REX, ModRM/SIB, the opcodes
 * above) from the Intel SDM Vol. 2 (chapter 2 and the instruction pages);
 * paging structures from Vol. 3 chapter 4. Original code. */

#include <asm/kvm.h>
#include <asm/vmx.h>
#include <uapi/kvm.h>
#include <kernel/errno.h>
#include <kernel/lib/string.h>
#include <kernel/mm/pmm.h>

#define PTE_P   1ULL
#define PTE_PS  (1ULL << 7)
#define ADDR52  0x000FFFFFFFFFF000ULL

static int rd64(kvm_vcpu_t *v, uint64_t gpa, uint64_t *e)
{
	return kvm_gpa_read(v->vm, gpa, e, 8, &v->prefault);
}

/* guest linear address to guest-physical, by the guest's paging mode */
static int walk(kvm_vcpu_t *v, uint64_t la, uint64_t *gpa)
{
	uint64_t cr3 = vmread(GUEST_CR3), e;
	int r;
	if (!(v->cr0 & (1ULL << 31))) { *gpa = la; return 0; }
	if (v->efer & EFER_LMA) {
		uint64_t t = cr3 & ADDR52;
		for (int shift = 39; shift >= 12; shift -= 9) {
			if ((r = rd64(v, t + ((la >> shift) & 511) * 8, &e))) return r;
			if (!(e & PTE_P)) return -EFAULT;
			if (shift == 12 || ((shift == 30 || shift == 21) && (e & PTE_PS))) {
				uint64_t mask = (1ULL << shift) - 1;
				*gpa = ((e & ADDR52) & ~mask) | (la & mask);
				return 0;
			}
			t = e & ADDR52;
		}
		return -EFAULT;
	}
	la &= 0xFFFFFFFFULL;
	if (v->cr4 & (1ULL << 5)) {   /* PAE: the PDPTEs are in the VMCS */
		e = vmread(GUEST_PDPTE0 + 2 * ((la >> 30) & 3));
		uint64_t t;
		for (int shift = 21; ; shift = 12) {
			if (!(e & PTE_P)) return -EFAULT;
			t = e & ADDR52;
			if ((r = rd64(v, t + ((la >> shift) & 511) * 8, &e))) return r;
			if (!(e & PTE_P)) return -EFAULT;
			if (shift == 12 || (e & PTE_PS)) {
				uint64_t mask = (1ULL << shift) - 1;
				*gpa = ((e & ADDR52) & ~mask) | (la & mask);
				return 0;
			}
		}
	}
	uint32_t pde, pte;
	if ((r = kvm_gpa_read(v->vm, (cr3 & 0xFFFFF000) + (la >> 22) * 4, &pde, 4, &v->prefault))) return r;
	if (!(pde & 1)) return -EFAULT;
	if ((pde & PTE_PS) && (v->cr4 & (1ULL << 4))) {   /* 4 MB page (CR4.PSE) */
		*gpa = (pde & 0xFFC00000ULL) | (la & 0x3FFFFF);
		return 0;
	}
	if ((r = kvm_gpa_read(v->vm, (pde & 0xFFFFF000) + ((la >> 12) & 0x3FF) * 4, &pte, 4, &v->prefault))) return r;
	if (!(pte & 1)) return -EFAULT;
	*gpa = (pte & 0xFFFFF000ULL) | (la & 0xFFF);
	return 0;
}

/* up to 15 instruction bytes at CS:RIP */
static int fetch(kvm_vcpu_t *v, uint8_t *buf, int *n)
{
	uint64_t rip = vmread(GUEST_RIP);
	uint64_t la = (v->efer & EFER_LMA) && (vmread(GUEST_ES_AR + 2 * SEG_CS) & (1U << 13))
	            ? rip : vmread(GUEST_ES_BASE + 2 * SEG_CS) + rip;
	int got = 0;
	while (got < 15) {
		uint64_t gpa;
		int r = walk(v, la + got, &gpa);
		if (r) return got ? 0 : r;   /* the rest of a short instruction may be unmapped */
		int chunk = (int)(4096 - (gpa & 4095));
		if (chunk > 15 - got) chunk = 15 - got;
		if ((r = kvm_gpa_read(v->vm, gpa, buf + got, (size_t)chunk, &v->prefault))) return got ? 0 : r;
		got += chunk;
	}
	*n = got;
	return 0;
}

/* Decode the access. Returns 0 and fills *d, or -1: not an instruction this
 * emulator handles. */
static int decode(kvm_vcpu_t *v, const uint8_t *b, int n, kvm_mmio_t *d)
{
	uint32_t cs_ar = (uint32_t)vmread(GUEST_ES_AR + 2 * SEG_CS);
	int long64 = (v->efer & EFER_LMA) && (cs_ar & (1U << 13));
	int def32 = long64 || ((v->cr0 & 1) && (cs_ar & (1U << 14)));
	int osz = def32 ? 4 : 2, asz = long64 ? 8 : (def32 ? 4 : 2);
	int rex = 0, i = 0;
	memset(d, 0, sizeof(*d));
	for (; i < n; i++) {   /* legacy prefixes */
		uint8_t p = b[i];
		if (p == 0x66) osz = def32 ? 2 : 4;
		else if (p == 0x67) asz = long64 ? 4 : (def32 ? 2 : 4);
		else if (p == 0x26 || p == 0x2E || p == 0x36 || p == 0x3E || p == 0x64 || p == 0x65 || p == 0xF0) continue;
		else break;
	}
	if (long64 && i < n && (b[i] & 0xF0) == 0x40) {
		rex = b[i++];
		if (rex & 8) osz = 8;
	}
	if (i >= n) return -1;
	uint8_t op = b[i++];
	int two = 0;
	if (op == 0x0F) {
		if (i >= n) return -1;
		two = 1;
		op = b[i++];
	}
	int has_modrm = 1, imm_bytes = 0;
	d->reg_size = (uint8_t)osz;
	if (!two) {
		switch (op) {
		case 0x88: d->write = 1; d->size = 1; break;
		case 0x89: d->write = 1; d->size = (uint8_t)osz; break;
		case 0x8A: d->size = 1; d->reg_size = 1; break;
		case 0x8B: d->size = (uint8_t)osz; break;
		case 0xC6: d->write = 1; d->size = 1; imm_bytes = 1; break;
		case 0xC7: d->write = 1; d->size = (uint8_t)osz; imm_bytes = osz == 2 ? 2 : 4; break;
		case 0xA0: case 0xA1: case 0xA2: case 0xA3:   /* accumulator <-> moffs */
			has_modrm = 0;
			d->write = op >= 0xA2;
			d->size = (op & 1) ? (uint8_t)osz : 1;
			d->reg_size = d->size;
			d->reg = R_AX;
			i += asz;
			break;
		default: return -1;
		}
	} else {
		switch (op) {
		case 0xB6: d->size = 1; break;
		case 0xB7: d->size = 2; break;
		case 0xBE: d->size = 1; d->sign = 1; break;
		case 0xBF: d->size = 2; d->sign = 1; break;
		default: return -1;
		}
	}
	if (has_modrm) {
		if (i >= n) return -1;
		uint8_t m = b[i++];
		int mod = m >> 6, reg = (m >> 3) & 7, rm = m & 7;
		if (mod == 3) return -1;   /* a register operand: not a memory access */
		if ((op == 0xC6 || op == 0xC7) && !two && reg != 0) return -1;
		d->reg = (int8_t)(reg | ((rex & 4) ? 8 : 0));
		/* AH, CH, DH, BH: byte registers 4-7 without a REX prefix */
		if (d->size == 1 && d->reg_size == 1 && !rex && reg >= 4) { d->high8 = 1; d->reg = (int8_t)(reg - 4); }
		if (asz == 2) {
			if (mod == 1) i += 1;
			else if (mod == 2 || (mod == 0 && rm == 6)) i += 2;
		} else {
			if (rm == 4) {
				if (i >= n) return -1;
				uint8_t sib = b[i++];
				if (mod == 0 && (sib & 7) == 5) i += 4;
			}
			if (mod == 1) i += 1;
			else if (mod == 2 || (mod == 0 && rm == 5)) i += 4;
		}
	}
	if (imm_bytes) {
		if (i + imm_bytes > n) return -1;
		uint64_t imm = 0;
		memcpy(&imm, b + i, (size_t)imm_bytes);
		if (imm_bytes == 4 && d->size == 8) imm = (uint64_t)(int64_t)(int32_t)imm;
		d->imm = imm;
		d->reg = -1;
		i += imm_bytes;
	}
	if (i > n) return -1;
	/* a load into RSP would have to reach the VMCS after the exit: refused */
	if (!d->write && d->reg == R_SP && !d->high8) return -1;
	d->len = (uint8_t)i;
	return 0;
}

static uint64_t reg_read(kvm_vcpu_t *v, const kvm_mmio_t *d)
{
	if (d->reg < 0) return d->imm;
	uint64_t r = d->reg == R_SP ? vmread(GUEST_RSP) : v->g.r[d->reg];
	return d->high8 ? r >> 8 : r;
}

/* The EPT violation at gpa hit no memory slot: decode the instruction and
 * send the access to userspace. RUN_* as handle_exit. */
int kvm_mmio_exit(kvm_vcpu_t *v, uint64_t gpa)
{
	uint8_t b[15];
	int n = 0;
	int r = fetch(v, b, &n);
	if (r == -EAGAIN) return KVM_MMIO_RETRY;   /* map the code page, run the access again */
	struct kvm_run *run = v->run;
	kvm_mmio_t d;
	if (r || decode(v, b, n, &d)) {
		run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
		run->internal.suberror = KVM_INTERNAL_ERROR_EMULATION;
		run->internal.ndata = 4;
		run->internal.data[0] = gpa;
		run->internal.data[1] = vmread(GUEST_RIP);
		memcpy(&run->internal.data[2], b, 15);   /* the bytes that did not decode */
		return KVM_MMIO_USER;
	}
	run->exit_reason = KVM_EXIT_MMIO;
	run->mmio.phys_addr = gpa;
	run->mmio.len = d.size;
	run->mmio.is_write = d.write;
	memset(run->mmio.data, 0, sizeof(run->mmio.data));
	if (d.write) {
		uint64_t val = reg_read(v, &d);
		if (kvm_ioeventfd_hit(v->vm, 0, gpa, d.size, val)) {
			vmwrite(GUEST_RIP, vmread(GUEST_RIP) + d.len);
			return KVM_MMIO_GUEST;
		}
		memcpy(run->mmio.data, &val, d.size);
	} else {
		v->mmio = d;
		v->mmio_pending = 1;
	}
	vmwrite(GUEST_RIP, vmread(GUEST_RIP) + d.len);
	return KVM_MMIO_USER;
}

static void load_reg(kvm_vcpu_t *v, const kvm_mmio_t *d, uint64_t val);

/* KVM_RUN after a load: put userspace's data in the register */
void kvm_mmio_finish(kvm_vcpu_t *v)
{
	if (!v->mmio_pending) return;
	v->mmio_pending = 0;
	uint64_t val = 0;
	memcpy(&val, v->run->mmio.data, v->mmio.size);
	load_reg(v, &v->mmio, val);
}

/* An access to the vcpu's own APIC page, done here: a load lands in its
 * register at once. A store that ends a level-triggered interrupt (EOI) goes
 * to userspace's IOAPIC as KVM_EXIT_IOAPIC_EOI. */
int kvm_mmio_apic(kvm_vcpu_t *v, uint64_t gpa)
{
	uint8_t b[15];
	int n = 0;
	int r = fetch(v, b, &n);
	if (r == -EAGAIN) return KVM_MMIO_RETRY;
	kvm_mmio_t d;
	if (r || decode(v, b, n, &d) || d.size > 4) {
		struct kvm_run *run = v->run;
		run->exit_reason = KVM_EXIT_INTERNAL_ERROR;
		run->internal.suberror = KVM_INTERNAL_ERROR_EMULATION;
		run->internal.ndata = 2;
		run->internal.data[0] = gpa;
		run->internal.data[1] = vmread(GUEST_RIP);
		return KVM_MMIO_USER;
	}
	uint32_t off = (uint32_t)(gpa & 0xFFF);
	vmwrite(GUEST_RIP, vmread(GUEST_RIP) + d.len);
	if (!d.write) {
		load_reg(v, &d, kvm_lapic_read(v, off));
		return KVM_MMIO_GUEST;
	}
	int vec = kvm_lapic_write(v, off, (uint32_t)reg_read(v, &d));
	if (vec < 0) return KVM_MMIO_GUEST;
	v->run->exit_reason = KVM_EXIT_IOAPIC_EOI;
	v->run->eoi.vector = (uint8_t)vec;
	return KVM_MMIO_USER;
}

/* a load's value into its register, sized and extended as the instruction says */
static void load_reg(kvm_vcpu_t *v, const kvm_mmio_t *d, uint64_t val)
{
	if (d->sign) val = d->size == 1 ? (uint64_t)(int64_t)(int8_t)val : (uint64_t)(int64_t)(int16_t)val;
	uint64_t *r = &v->g.r[d->reg];   /* never RSP: no MMIO load targets the stack pointer here */
	if (d->high8) { *r = (*r & ~0xFF00ULL) | ((val & 0xFF) << 8); return; }
	switch (d->reg_size) {
	case 1: *r = (*r & ~0xFFULL) | (val & 0xFF); break;
	case 2: *r = (*r & ~0xFFFFULL) | (val & 0xFFFF); break;
	case 4: *r = val & 0xFFFFFFFFULL; break;   /* a 32-bit destination clears the upper half */
	default: *r = val;
	}
}

/* ── string port I/O ─────────────────────────────────────── */

/* INS and OUTS, with or without REP, as KVM_EXIT_IO with count elements in
 * the data page: as many as lie on the memory operand's page, so one exit moves
 * at most a page and needs one translation. A REP with more to do leaves RIP on
 * the instruction, which runs again for the rest. OUTS data is copied out now;
 * INS data goes to guest memory at the next KVM_RUN (kvm_pio_string_finish).
 * The memory operand's linear address is the VMCS's (guest linear address);
 * the address size comes from the VM-exit instruction information. */
int kvm_pio_string(kvm_vcpu_t *v, uint64_t q)
{
	uint32_t size = (uint32_t)(q & 7) + 1;
	int in = (q >> 3) & 1, rep = (q >> 5) & 1;
	uint32_t info = (uint32_t)vmread(VMX_INSTRUCTION_INFO);
	int asz = (info >> 7) & 7;   /* 0: 16-bit, 1: 32-bit, 2: 64-bit */
	uint64_t amask = asz == 2 ? ~0ULL : asz == 1 ? 0xFFFFFFFFULL : 0xFFFFULL;
	uint64_t la = vmread(GUEST_LINEAR_ADDRESS);
	int down = (vmread(GUEST_RFLAGS) >> 10) & 1;   /* DF */
	uint64_t rcx = v->g.r[R_CX] & amask;
	if (rep && rcx == 0) {
		vmwrite(GUEST_RIP, vmread(GUEST_RIP) + vmread(VM_EXIT_INSTRUCTION_LEN));
		return KVM_MMIO_GUEST;
	}
	uint64_t off = la & 4095;
	if (off + size > 4096) return KVM_MMIO_USER_ERROR;   /* an element across a page: not done */
	uint64_t fit = down ? off / size + 1 : (4096 - off) / size;
	uint64_t count = rep ? (rcx < fit ? rcx : fit) : 1;
	uint64_t gpa;
	int r = walk(v, la, &gpa);
	if (r == -EAGAIN) return KVM_MMIO_RETRY;
	if (r) return KVM_MMIO_USER_ERROR;
	uint64_t lo = down ? gpa - (count - 1) * size : gpa;   /* the block, lowest address first */
	uint8_t *data = (uint8_t *)PHYS_TO_VIRT(v->pio_page);
	if (in) {
		uint8_t probe;
		if ((r = kvm_gpa_read(v->vm, lo, &probe, 1, &v->prefault)) == -EAGAIN) return KVM_MMIO_RETRY;
		v->ins_gpa = lo;
		v->ins_bytes = (uint32_t)(count * size);
		v->ins_size = (uint8_t)size;
		v->ins_down = (uint8_t)down;
	} else {
		if ((r = kvm_gpa_read(v->vm, lo, data, count * size, &v->prefault)) == -EAGAIN) return KVM_MMIO_RETRY;
		if (r) return KVM_MMIO_USER_ERROR;
		if (down)   /* the elements in the order the instruction sends them */
			for (uint64_t i = 0; i < count / 2; i++)
				for (uint32_t b = 0; b < size; b++) {
					uint8_t t = data[i * size + b];
					data[i * size + b] = data[(count - 1 - i) * size + b];
					data[(count - 1 - i) * size + b] = t;
				}
	}
	int idx = in ? R_DI : R_SI;
	uint64_t delta = count * size, reg = v->g.r[idx];
	reg = down ? reg - delta : reg + delta;
	v->g.r[idx] = (v->g.r[idx] & ~amask) | (reg & amask);
	if (rep) v->g.r[R_CX] = (v->g.r[R_CX] & ~amask) | ((rcx - count) & amask);
	if (!rep || rcx == count)
		vmwrite(GUEST_RIP, vmread(GUEST_RIP) + vmread(VM_EXIT_INSTRUCTION_LEN));
	struct kvm_run *run = v->run;
	run->exit_reason = KVM_EXIT_IO;
	run->io.direction = in ? KVM_EXIT_IO_IN : KVM_EXIT_IO_OUT;
	run->io.size = (uint8_t)size;
	run->io.port = (uint16_t)(q >> 16);
	run->io.count = (uint32_t)count;
	run->io.data_offset = PAGE_SIZE;
	return KVM_MMIO_USER;
}

int kvm_pio_string_finish(kvm_vcpu_t *v)
{
	if (!v->ins_bytes) return 0;
	uint8_t *data = (uint8_t *)PHYS_TO_VIRT(v->pio_page);
	uint32_t n = v->ins_bytes, size = v->ins_size;
	if (v->ins_down)   /* userspace gave them in execution order: highest address first */
		for (uint32_t i = 0; i < n / size / 2; i++)
			for (uint32_t b = 0; b < size; b++) {
				uint8_t t = data[i * size + b];
				data[i * size + b] = data[n - (i + 1) * size + b];
				data[n - (i + 1) * size + b] = t;
			}
	v->ins_bytes = 0;
	for (int tries = 0; tries < 3; tries++) {
		uint64_t missing;
		int r = kvm_gpa_write(v->vm, v->ins_gpa, data, n, &missing);
		if (r != -EAGAIN) return r;
		if ((r = kvm_ept_fault(v->vm, missing)) <= 0) return r ? r : -EFAULT;
	}
	return -EFAULT;
}
