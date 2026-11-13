/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/*
 * Suspend and resume: the order, the clocks, the files. The machine side is
 * arch/x86_64/sleep.c; the design ozel-SUSP1/TASARIM.md.
 *
 *   /sys/power/state            "freeze mem"; root writes one to sleep, the write
 *                               returns after the resume
 *   /sys/power/mem_sleep        "[s2idle]", or "[s2idle] deep" with S3 allowed
 *   /sys/power/suspend_stats/   success, fail, last_failed_step (Linux's names)
 *   /sys/power/ranix_last       "kind=s2idle slept_ms=4012 wake=rtc"
 *   /sys/class/rtc/rtc0/wakealarm   "+N", an epoch, or 0 (what rtcwake writes)
 *
 * The order (before the CPUs are parked, everything that may sleep or lock):
 * sync the disks, the drivers' suspend in reverse order, the watchdog halted,
 * the vDSO onto the system call, the task onto CPU 0, the other CPUs parked;
 * then the sleep; then the raw clock re-anchored at the monotonic time of the
 * entry (MONOTONIC does not count the sleep, BOOTTIME and REALTIME do), the
 * CPUs back, the vDSO, the watchdog, the drivers' resume in order.
 */
#include <uapi/types.h>
#include <kernel/sleep.h>
#include <kernel/block.h>
#include <kernel/capabilities.h>
#include <kernel/cmdline.h>
#include <kernel/cpumask.h>
#include <kernel/errno.h>
#include <kernel/hal.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#include <kernel/proc.h>
#include <kernel/smp.h>
#include <kernel/time.h>
#include <kernel/vdso.h>
#include <kernel/vfs.h>
#include <kernel/config.h>
#include <kernel/watchdog.h>

/* The arch half; a machine without one (aarch64 for now) offers nothing. */
__attribute__((weak)) int arch_sleep_deep_ok(void) { return 0; }
__attribute__((weak)) const char *arch_sleep_deep_why(void) { return "no suspend on this architecture"; }
__attribute__((weak)) int arch_sleep_park_cpus(int kind) { (void)kind; return -EINVAL; }
__attribute__((weak)) void arch_sleep_release_cpus(int kind) { (void)kind; }
__attribute__((weak)) int arch_sleep_enter(int kind, uint64_t *rb, uint64_t *ra, int64_t *tb, int64_t *ta)
{ (void)kind; (void)rb; (void)ra; (void)tb; (void)ta; return -EINVAL; }
__attribute__((weak)) void arch_sleep_after(void) { }
__attribute__((weak)) void arch_clock_rebase(uint64_t ns) { (void)ns; }
__attribute__((weak)) uint64_t arch_clock_raw_ns(void) { return mono_ns(); }
__attribute__((weak)) int rtc_wakealarm_set(int64_t at) { (void)at; return -EINVAL; }
__attribute__((weak)) int64_t rtc_wakealarm_get(void) { return 0; }

static int arch_has_sleep(void)
{
#if defined(__x86_64__)
	return 1;
#else
	return 0;
#endif
}

/* ---- the drivers ---- */

#define PM_MAX 32
static pm_ops_t *pm_list[PM_MAX];
static int pm_n;

void pm_register(pm_ops_t *ops)
{
	if (pm_n < PM_MAX) pm_list[pm_n++] = ops;
	else kprintf(KERN_WARN "PM: no room for %s\n", ops->name);
}

/* deep only: a device that loses its state in S3 and has nobody to restore it */
static const char *pm_blocker(void)
{
	for (int i = 0; i < pm_n; i++)
		if (pm_list[i]->powers_off && !pm_list[i]->resume) return pm_list[i]->name;
	return 0;
}

/* ---- the sleep ---- */

static volatile int busy;
static volatile int in_progress;
static int mem_default = SLEEP_S2IDLE;
static uint32_t n_success, n_fail;
static const char *failed_step = "";
static int last_kind;
static uint64_t last_slept_ms;
static int last_wake = -1;

int sleep_in_progress(void) { return in_progress; }

static const char *kind_name(int k) { return k == SLEEP_DEEP ? "deep" : "s2idle"; }

static const char *wake_name(int w)
{
	switch (w) {
	case WAKE_RTC: return "rtc";
	case WAKE_POWER: return "power";
	case WAKE_KEY: return "key";
	case WAKE_SCI: return "sci";
	case WAKE_OTHER: return "other";
	default: return "none";
	}
}

static int fail(const char *step, int err)
{
	n_fail++;
	failed_step = step;
	kprintf(KERN_WARN "PM: suspend failed at %s (%d)\n", step, err);
	return err;
}

/* the current task onto CPU 0, where the firmware resumes and the 8259 delivers */
static cpumask_t *pin_cpu0(void)
{
	process_t *p = task_current();
	cpumask_t *old = p->cpu_affinity, *m = kmalloc(sizeof(*m));
	if (!m) return old;
	cpumask_clear(m);
	cpumask_set_cpu(0, m);
	p->cpu_affinity = m;
	for (int i = 0; i < 1000 && arch_get_cpu_ctx()->cpu_id != 0; i++) yield();
	return old;
}

static void unpin(cpumask_t *old)
{
	process_t *p = task_current();
	cpumask_t *m = p->cpu_affinity;
	p->cpu_affinity = old;
	if (m && m != old) kfree(m);
}

int sleep_enter(int kind)
{
	if (!arch_has_sleep()) return -EINVAL;
	if (kind != SLEEP_S2IDLE && kind != SLEEP_DEEP) return -EINVAL;
	if (kind == SLEEP_DEEP && !arch_sleep_deep_ok()) return -EINVAL;
	if (__sync_lock_test_and_set(&busy, 1)) return -EBUSY;

	int err = 0, i;
	const char *b = kind == SLEEP_DEEP ? pm_blocker() : 0;
	char v[8];
	if (b && cmdline_get("ranix.s3", v, sizeof v) && !strcmp(v, "force")) {
		/* tests only: the S3 path itself on a machine with such a device unused */
		kprintf(KERN_WARN "PM: %s has no resume, S3 anyway (ranix.s3=force)\n", b);
		b = 0;
	}
	if (b) {
		kprintf(KERN_WARN "PM: %s has no resume, no S3\n", b);
		err = fail("suspend_devices", -EBUSY);
		goto out;
	}
	kprintf(KERN_INFO "PM: suspend entry (%s)\n", kind_name(kind));

#if IS_ENABLED(CONFIG_FS_EXT2)
	ext2_sync_all();
#endif
	block_flush_all();

	for (i = pm_n - 1; i >= 0; i--) {
		if (!pm_list[i]->suspend) continue;
		int e = pm_list[i]->suspend(kind);
		if (e) {
			kprintf(KERN_WARN "PM: %s refused to suspend (%d)\n", pm_list[i]->name, e);
			for (int j = i + 1; j < pm_n; j++)
				if (pm_list[j]->resume) pm_list[j]->resume(kind);
			err = fail("suspend_devices", e);
			goto out;
		}
	}

	cpumask_t *old = pin_cpu0();
#if defined(__x86_64__)
	watchdog_sleep();
#endif
	vdso_sleep(1);
	in_progress = 1;

	int w = -EBUSY;
	uint64_t rb = 0, ra = 0;
	int64_t tb = 0, ta = 0;
	if (arch_get_cpu_ctx()->cpu_id != 0) {
		err = fail("pin_cpu0", -EBUSY);
	} else if ((err = arch_sleep_park_cpus(kind)) != 0) {
		fail("park_cpus", err);
	} else {
		w = arch_sleep_enter(kind, &rb, &ra, &tb, &ta);
		if (w >= 0) {
			uint64_t slept = sleep_length_ns(kind, rb, ra, tb, ta);
			arch_clock_rebase(rb);
			clock_sleep_resume(rb, slept);
			arch_sleep_after();
			last_kind = kind;
			last_slept_ms = slept / 1000000ULL;
			last_wake = w;
		} else {
			err = fail("enter", w);
		}
		arch_sleep_release_cpus(kind);
	}

	in_progress = 0;
	vdso_sleep(0);
#if defined(__x86_64__)
	watchdog_wake();
#endif
	for (i = 0; i < pm_n; i++)
		if (pm_list[i]->resume) pm_list[i]->resume(kind);
	unpin(old);

	if (w >= 0) {
		if (w == WAKE_RTC) rtc_wakealarm_set(0);   /* fired: the file reads empty, as on Linux */
		n_success++;
		kprintf(KERN_INFO "PM: resume from %s, slept %lu ms, wake %s\n", kind_name(kind),
		        (unsigned long)last_slept_ms, wake_name(w));
		err = 0;
	}
out:
	__sync_lock_release(&busy);
	return err;
}

/* ---- the files ---- */

enum { N_ROOT = 1, N_STATE, N_MEMSLEEP, N_LAST, N_STATS, N_SUCCESS, N_FAIL, N_STEP,
       N_RTCCLASS, N_RTC0, N_WAKEALARM, N_RTCNAME, N_MAX };

static const struct { int parent; const char *name; uint32_t mode; } nodes[N_MAX] = {
	[N_STATE]     = { N_ROOT, "state", S_IFREG | 0644 },
	[N_MEMSLEEP]  = { N_ROOT, "mem_sleep", S_IFREG | 0644 },
	[N_LAST]      = { N_ROOT, "ranix_last", S_IFREG | 0444 },
	[N_STATS]     = { N_ROOT, "suspend_stats", S_IFDIR | 0555 },
	[N_SUCCESS]   = { N_STATS, "success", S_IFREG | 0444 },
	[N_FAIL]      = { N_STATS, "fail", S_IFREG | 0444 },
	[N_STEP]      = { N_STATS, "last_failed_step", S_IFREG | 0444 },
	[N_RTC0]      = { N_RTCCLASS, "rtc0", S_IFDIR | 0555 },
	[N_WAKEALARM] = { N_RTC0, "wakealarm", S_IFREG | 0644 },
	[N_RTCNAME]   = { N_RTC0, "name", S_IFREG | 0444 },
};

static vfs_iops_t pdir_iops;
static vfs_fops_t pfile_fops, pdir_fops;

static vfs_inode_t *mk(int id)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino = 0xd6000000ULL + (uint64_t)id;
	in->refcount = 1;
	in->priv = (void *)(uintptr_t)id;
	in->priv_static = 1;
	uint32_t mode = id == N_ROOT || id == N_RTCCLASS ? (S_IFDIR | 0555) : nodes[id].mode;
	in->mode = mode;
	if (S_ISDIR(mode)) {
		in->nlink = 2; in->iops = &pdir_iops; in->fops = &pdir_fops; in->no_cache = 1;
	} else {
		in->nlink = 1; in->fops = &pfile_fops; in->size = 4096;
	}
	return in;
}

static int dir_id(vfs_inode_t *d) { return (int)(uintptr_t)d->priv; }

static vfs_inode_t *pdir_lookup(vfs_inode_t *dir, const char *name)
{
	for (int i = 1; i < N_MAX; i++)
		if (nodes[i].name && nodes[i].parent == dir_id(dir) && !strcmp(nodes[i].name, name)) return mk(i);
	return NULL;
}

static int pdir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	uint32_t k = 0;
	for (int i = 1; i < N_MAX; i++) {
		if (!nodes[i].name || nodes[i].parent != dir_id(dir)) continue;
		if (k++ != idx) continue;
		out->ino = 0xd6000000ULL + (uint64_t)i;
		out->mode = nodes[i].mode;
		out->next_off = idx + 1;
		memcpy(out->name, nodes[i].name, strlen(nodes[i].name) + 1);
		return 0;
	}
	return -1;
}

static int put(char *o, int n, const char *s)
{
	size_t k = strlen(s);
	memcpy(o + n, s, k);
	return n + (int)k;
}

static int put_u(char *o, int n, uint64_t v)
{
	char t[24];
	int k = 0;
	do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (k) o[n++] = t[--k];
	return n;
}

static int fill(int id, char *t)
{
	int n = 0;
	switch (id) {
	case N_STATE:     n = arch_has_sleep() ? put(t, 0, "freeze mem\n") : put(t, 0, "\n"); break;
	case N_MEMSLEEP:  n = sleep_format_mem_sleep(t, 64, mem_default, arch_sleep_deep_ok()); break;
	case N_LAST:
		if (last_wake < 0) { n = put(t, 0, "none\n"); break; }
		n = put(t, 0, "kind="); n = put(t, n, kind_name(last_kind));
		n = put(t, n, " slept_ms="); n = put_u(t, n, last_slept_ms);
		n = put(t, n, " wake="); n = put(t, n, wake_name(last_wake));
		t[n++] = '\n';
		break;
	case N_SUCCESS:   n = put_u(t, 0, n_success); t[n++] = '\n'; break;
	case N_FAIL:      n = put_u(t, 0, n_fail); t[n++] = '\n'; break;
	case N_STEP:      n = put(t, 0, failed_step); t[n++] = '\n'; break;
	case N_WAKEALARM: {
		int64_t a = rtc_wakealarm_get();
		if (a > 0) { n = put_u(t, 0, (uint64_t)a); t[n++] = '\n'; }
		break;
	}
	case N_RTCNAME:   n = put(t, 0, "rtc_cmos\n"); break;
	}
	return n < 0 ? 0 : n;
}

static ssize_t pfile_read(vfs_file_t *f, void *buf, size_t len)
{
	char t[128];
	int n = fill(dir_id(f->inode), t);
	if (f->pos >= n) return 0;
	size_t c = (size_t)(n - f->pos);
	if (c > len) c = len;
	if (copy_fop_to_user(buf, t + f->pos, c) != 0) return -EFAULT;
	f->pos += (off_t)c;
	return (ssize_t)c;
}

static ssize_t pfile_write(vfs_file_t *f, const void *buf, size_t len)
{
	int id = dir_id(f->inode);
	if (id != N_STATE && id != N_MEMSLEEP && id != N_WAKEALARM) return -EINVAL;
	if (!capable_task(task_current(), CAP_SYS_ADMIN)) return -EPERM;
	char k[32];
	size_t n = len < sizeof(k) ? len : sizeof(k);
	if (copy_fop_from_user(k, buf, n) != 0) return -EFAULT;
	if (id == N_MEMSLEEP) {
		int m = sleep_parse_mem_sleep(k, n);
		if (m < 0 || (m == SLEEP_DEEP && !arch_sleep_deep_ok())) return -EINVAL;
		mem_default = m;
		return (ssize_t)len;
	}
	if (id == N_WAKEALARM) {
		int64_t at, s, ns;
		ranix_walltime(&s, &ns);
		if (wakealarm_parse(k, n, s, &at)) return -EINVAL;
		if (at && rtc_wakealarm_get() > 0) return -EBUSY;    /* as Linux: clear with 0 first */
		int e = rtc_wakealarm_set(at);
		return e ? e : (ssize_t)len;
	}
	int kind = sleep_parse_state(k, n, mem_default);
	if (kind < 0) return kind;
	int e = sleep_enter(kind);
	return e ? e : (ssize_t)len;
}

static vfs_iops_t pdir_iops = { .lookup = pdir_lookup, .readdir = pdir_readdir };
static vfs_fops_t pfile_fops = { .read = pfile_read, .write = pfile_write };
static vfs_fops_t pdir_fops = { 0 };

vfs_inode_t *power_sysfs_root(void) { return mk(N_ROOT); }
vfs_inode_t *rtc_sysfs_root(void) { return mk(N_RTCCLASS); }
