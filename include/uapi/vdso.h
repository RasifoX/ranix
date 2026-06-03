#ifndef _UAPI_VDSO_H
#define _UAPI_VDSO_H

/* Shared by the kernel (-nostdinc) and the userspace vDSO build, so use base C
 * types only — no <stdint.h>. 64-bit fields are long long; the LP64 mismatch
 * with the kernel's int64_t is type-only, the layout is identical. */

/* Fixed user VAs for the vDSO mapping, in the gap between the program image
 * (~0x400000) and the mmap base (0x40000000) — neither brk nor mmap reaches
 * here, so no ASLR/collision handling is needed for a first version. The vDSO
 * reads vvar through the absolute VVAR address (no relocation, PIC-safe). */
#define VVAR_VADDR  0x3FFF0000UL
#define VDSO_VADDR  0x3FFF1000UL   /* AT_SYSINFO_EHDR; image fits below mmap base 0x40000000 */

/* Timekeeping page, written by the kernel and read by the vDSO without a
 * syscall. A seqlock (odd = update in progress) lets the reader retry on a torn
 * update. Times are a base captured at the last kernel update plus, when
 * cnt_valid, an offset from the hardware counter — sub-tick resolution where
 * the counter is user-readable (aarch64 cntvct); tick-granular but monotonic
 * otherwise (x86 until TSC is calibrated). */
struct vdso_vvar {
	volatile unsigned int seq;
	unsigned int          cnt_valid;
	long long             mono_sec;
	long long             mono_nsec;
	long long             real_sec;
	long long             real_nsec;
	unsigned long long    cnt_base;
	unsigned int          cnt_mult;   /* delta_ns = (delta_cnt * mult) >> shift */
	unsigned int          cnt_shift;
};

#endif
