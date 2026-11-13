/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef KERNEL_SLEEP_H
#define KERNEL_SLEEP_H

/* Suspend and resume (kernel/suspend.c, arch/x86_64/sleep.c, kernel/lib/sleep_math.c).
 * The design: ozel-SUSP1/TASARIM.md. Two kinds, named as Linux names them:
 *   s2idle  devices stay powered, the CPUs idle, an interrupt wakes (K1)
 *   deep    ACPI S3: RAM in self refresh, the firmware jumps back to us (K2, ranix.s3=1)
 * CLOCK_MONOTONIC stands still while asleep; BOOTTIME and REALTIME move on. */

#ifdef HOST_TEST
#include <stdint.h>
#include <stddef.h>
#else
#include <uapi/types.h>
#endif

#define SLEEP_S2IDLE 1
#define SLEEP_DEEP   3

/* ---- pure (kernel/lib/sleep_math.c, checked by tests/host_sleep.c) ---- */

#define SLEEP_TYPE_NONE 0   /* no \_Sx_ package, or it sits under If (C) with C = Zero */
#define SLEEP_TYPE_OK   1
#define SLEEP_TYPE_COND 2   /* found, under an If whose predicate is not a constant Name */

/* \_Sx_ from the DSDT bytes (header included), SLP_TYPa and SLP_TYPb in a and b. */
int acpi_sleep_type(const uint8_t *aml, uint32_t len, int state, uint8_t *a, uint8_t *b);

/* How long it slept. raw_* are the raw counter's ns at entry and at exit, before
 * re-anchoring (a counter that went back was reset: S3); rtc_* Unix seconds, 0 if
 * unreadable. The counter when it is continuous and within 2 s of the RTC. */
uint64_t sleep_length_ns(int kind, uint64_t raw_before, uint64_t raw_after, int64_t rtc_before, int64_t rtc_after);

typedef struct {
	uint64_t anchor_ns;                 /* the raw clock restarts here: monotonic at entry */
	int64_t  boot_off_sec, boot_off_nsec;   /* BOOTTIME - MONOTONIC */
	int64_t  rt_off_sec, rt_off_nsec;       /* REALTIME - MONOTONIC (the caller's own copy) */
	uint64_t slept_total_ns;
	uint32_t count;
} sleep_clock_t;

/* The clocks after a sleep of slept_ns that began at monotonic mono_at_entry_ns. */
void sleep_clock_resume(sleep_clock_t *c, uint64_t mono_at_entry_ns, uint64_t slept_ns);

/* "freeze" / "mem" (= mem_default) -> kind, <0 if not one of ours */
int sleep_parse_state(const char *s, size_t n, int mem_default);
int sleep_parse_mem_sleep(const char *s, size_t n);
/* "[s2idle] deep\n" and the like; the length, or <0 */
int sleep_format_mem_sleep(char *out, size_t n, int chosen, int deep_ok);
/* rtc wakealarm: "+N" relative to now, an absolute epoch, or "0" to clear (*at = 0) */
int wakealarm_parse(const char *s, size_t n, int64_t now, int64_t *at);

#ifndef HOST_TEST
/* ---- kernel ---- */

/* A driver's part. suspend may sleep (it runs before the CPUs are parked); a
 * nonzero return aborts the sleep and resumes those already suspended. resume
 * runs after the CPUs are back. A driver bound to a device that powers off in
 * S3 and has no resume blocks deep (it is named in the log). */
typedef struct pm_ops {
	const char *name;
	int  (*suspend)(int kind);
	void (*resume)(int kind);
	int  powers_off;                    /* its device loses state in S3 */
	struct pm_ops *next;
} pm_ops_t;

void pm_register(pm_ops_t *ops);
int  sleep_enter(int kind);             /* 0 after the resume, or -errno */
int  sleep_in_progress(void);           /* 1 between the parking of the CPUs and their release */

struct vfs_inode;
struct vfs_inode *power_sysfs_root(void);     /* /sys/power */
struct vfs_inode *rtc_sysfs_root(void);       /* /sys/class/rtc */

/* the clock side (kernel/sys/syscall_proc.c) */
void clock_sleep_resume(uint64_t mono_at_entry_ns, uint64_t slept_ns);
void clock_boot_offset(int64_t *sec, int64_t *nsec);

/* the arch side (arch/x86_64/sleep.c) */
#define WAKE_NONE    0
#define WAKE_RTC     1
#define WAKE_POWER   2
#define WAKE_KEY     3
#define WAKE_SCI     4
#define WAKE_OTHER   5
int  arch_sleep_deep_ok(void);          /* S3 possible on this machine and allowed */
const char *arch_sleep_deep_why(void);  /* why not, for the log */
int  arch_sleep_park_cpus(int kind);    /* 0, or -EBUSY when one did not park */
void arch_sleep_release_cpus(int kind);
int  arch_sleep_enter(int kind, uint64_t *raw_before, uint64_t *raw_after, int64_t *rtc_before, int64_t *rtc_after);
                                        /* the wake source, or -errno if it did not sleep */
void arch_sleep_after(void);          /* the BSP's tick again, IRQs on (after the clock is re-anchored) */
void arch_sleep_init(void);
void arch_clock_rebase(uint64_t ns);    /* the raw clock reads ns now (arch/x86_64/acpi.c) */
uint64_t arch_clock_raw_ns(void);       /* the raw clock, not yet re-anchored */
int  rtc_wakealarm_set(int64_t at);     /* 0 clears; arch/x86_64/sleep.c */
int64_t rtc_wakealarm_get(void);
#endif

#endif
