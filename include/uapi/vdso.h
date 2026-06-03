#ifndef _UAPI_VDSO_H
#define _UAPI_VDSO_H

#include <stdint.h>

/* Shared timekeeping page (the "vvar"), written by the kernel and read by the
 * vDSO in user space without a syscall. A seqlock (odd = update in progress)
 * lets the reader retry on a torn update. Times are a base captured at the last
 * kernel update plus, when cnt_valid, an offset derived from the hardware
 * counter — giving sub-tick resolution where the counter is user-readable
 * (aarch64 cntvct). Where it isn't (x86 until TSC is calibrated) cnt_valid is 0
 * and the base alone is returned (tick-granular but monotonic and correct). */
/* Fixed user VAs for the vDSO mapping, in the gap between the program image
 * (~0x400000) and the mmap base (0x40000000) — neither brk nor mmap reaches
 * here, so no ASLR/collision handling is needed for a first version. The vDSO
 * reads vvar through the absolute VVAR address (no relocation, PIC-safe). */
#define VVAR_VADDR  0x3FFFE000UL
#define VDSO_VADDR  0x3FFFF000UL   /* AT_SYSINFO_EHDR points here */

struct vdso_vvar {
	volatile uint32_t seq;        /* seqlock */
	uint32_t          cnt_valid;  /* 1: hardware-counter read is usable */
	int64_t           mono_sec;   /* CLOCK_MONOTONIC base at last update */
	int64_t           mono_nsec;
	int64_t           real_sec;   /* CLOCK_REALTIME base at last update */
	int64_t           real_nsec;
	uint64_t          cnt_base;   /* hardware counter at last update */
	uint32_t          cnt_mult;   /* delta_ns = (delta_cnt * mult) >> shift */
	uint32_t          cnt_shift;
};

#endif
