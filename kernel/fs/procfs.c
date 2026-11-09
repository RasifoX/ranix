/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/smp.h>
#include <kernel/fscred.h>
#include <kernel/vfs.h>
#include <kernel/proc.h>
#include <kernel/capabilities.h>
#include <kernel/cmdline.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/hugetlb.h>
#include <kernel/mm/heap.h>
#include <kernel/mm/mmu.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>
#include <kernel/sync.h>
#include <kernel/net/socket.h>
#include <kernel/net/net.h>
#include <kernel/net/ipv6.h>
#include <kernel/block.h>
#include <kernel/hal.h>

#include <kernel/nsproxy.h>
#include <kernel/asess.h>
#include <kernel/cgroup.h>
#include <kernel/irq.h>
#include <kernel/hwdiag.h>
#include <kernel/ksyms.h>
#include <kernel/time.h>
#include <kernel/vdso.h>
#include <kernel/watchdog.h>

/*
 * procfs — virtual filesystem at /proc.
 *
 * Content is generated dynamically on every read; no data is stored.
 * pb_t (pointer-buffer) provides snprintf-free string building.
 */

/* ── pb_t: minimal string builder ───────────────────────────── */

typedef struct { char *p; size_t rem; } pb_t;

static void pb_char(pb_t *b, char c)
{
	if (!b->rem) return;
	*b->p++ = c; b->rem--;
}

static void pb_str(pb_t *b, const char *s)
{
	while (*s) pb_char(b, *s++);
}

static void pb_u64(pb_t *b, uint64_t v)
{
	char tmp[20]; int i = 0;
	if (v == 0) { pb_char(b, '0'); return; }
	while (v > 0 && i < 20) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
	for (int j = i - 1; j >= 0; j--) pb_char(b, tmp[j]);
}

static void pb_u64kib(pb_t *b, uint64_t bytes)
{
	pb_u64(b, bytes / 1024);
	pb_str(b, " kB\n");
}

static void pb_hex16(pb_t *b, uint64_t v)
{
	static const char hx[] = "0123456789abcdef";
	for (int i = 15; i >= 0; i--)
		pb_char(b, hx[(v >> (i * 4)) & 0xf]);
}

/* ── fill callbacks ──────────────────────────────────────────── */

static int fill_version(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "Ranix version 6.6.0-ranix (ranix@ranix) "
	           "(gcc version 12.0) #1 SMP\n");
	return (int)(sz - b.rem);
}

static int fill_cpuinfo(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	for (int i = 0; i < smp_ncpus; i++) {
#if defined(__aarch64__)
		pb_str(&b, "processor\t: "); pb_u64(&b, i); pb_char(&b, '\n');
		pb_str(&b, "BogoMIPS\t: 100.00\n");
		pb_str(&b, "Features\t: fp asimd\n");
		pb_str(&b, "CPU implementer\t: 0x41\n");
		pb_str(&b, "CPU architecture: 8\n");
		pb_str(&b, "CPU variant\t: 0x0\n");
		pb_str(&b, "CPU part\t: 0xd08\n");
		pb_str(&b, "CPU revision\t: 3\n\n");
#else
		pb_str(&b, "processor\t: "); pb_u64(&b, i); pb_char(&b, '\n');
		{
			uint32_t r[12], a, bx, c, d;
#define CPUID(l) __asm__ volatile("cpuid" : "=a"(a), "=b"(bx), "=c"(c), "=d"(d) : "a"(l), "c"(0))
			char v[13], br[49];
			CPUID(0);
			((uint32_t *)v)[0] = bx; ((uint32_t *)v)[1] = d; ((uint32_t *)v)[2] = c;
			v[12] = 0;
			pb_str(&b, "vendor_id\t: "); pb_str(&b, v); pb_char(&b, '\n');
			CPUID(1);
			uint32_t fam = (a >> 8) & 15, mod = (a >> 4) & 15;
			if (fam == 15) fam += (a >> 20) & 0xFF;
			if (fam == 6 || fam >= 15) mod |= (a >> 12) & 0xF0;
			uint32_t step = a & 15;
			pb_str(&b, "cpu family\t: "); pb_u64(&b, fam); pb_char(&b, '\n');
			pb_str(&b, "model\t\t: "); pb_u64(&b, mod); pb_char(&b, '\n');
			for (uint32_t l = 0; l < 3; l++) {
				CPUID(0x80000002 + l);
				r[l * 4] = a; r[l * 4 + 1] = bx; r[l * 4 + 2] = c; r[l * 4 + 3] = d;
			}
			memcpy(br, r, 48);
			br[48] = 0;
			char *bp = br;
			while (*bp == ' ') bp++;
			pb_str(&b, "model name\t: "); pb_str(&b, bp); pb_char(&b, '\n');
			pb_str(&b, "stepping\t: "); pb_u64(&b, step); pb_char(&b, '\n');
#undef CPUID
		}
		pb_str(&b, "cpu MHz\t\t: 1000.000\n");
		pb_str(&b, "cache size\t: 4096 KB\n");
		pb_str(&b, "bogomips\t: 2000.00\n\n");
#endif
	}
	return (int)(sz - b.rem);
}

static int fill_meminfo(char *buf, size_t sz)
{
	uint64_t total = buddy_total_pages() * PAGE_SIZE;
	uint64_t free  = pmm_free_pages()    * PAGE_SIZE;
	uint64_t used  = total - free;
	pb_t b = {buf, sz};
	pb_str(&b, "MemTotal:       "); pb_u64kib(&b, total);
	pb_str(&b, "MemFree:        "); pb_u64kib(&b, free);
	pb_str(&b, "MemAvailable:   "); pb_u64kib(&b, free);
	pb_str(&b, "Buffers:        "); pb_u64kib(&b, 0);
	pb_str(&b, "Cached:         "); pb_u64kib(&b, 0);
	pb_str(&b, "SwapTotal:      "); pb_u64kib(&b, 0);
	pb_str(&b, "SwapFree:       "); pb_u64kib(&b, 0);
	pb_str(&b, "Active:         "); pb_u64kib(&b, used);
	pb_str(&b, "Inactive:       "); pb_u64kib(&b, 0);
	pb_str(&b, "AnonHugePages:  "); pb_u64kib(&b, huge2m_kb() * 1024);
	return (int)(sz - b.rem);
}

static int fill_mounts(char *buf, size_t sz)
{
	return (int)vfs_proc_mounts(buf, sz);
}

#include <kernel/board.h>
#include <kernel/fs/procfs.h>
#include <kernel/errno.h>

static int fill_uptime(char *buf, size_t sz)
{
	uint64_t secs = kernel_ticks / TIMER_HZ;
	uint64_t centi = ((kernel_ticks % TIMER_HZ) * 100) / TIMER_HZ;
	pb_t b = {buf, sz};
	pb_u64(&b, secs); pb_char(&b, '.'); pb_u64(&b, centi);
	pb_str(&b, " ");
	pb_u64(&b, secs); pb_char(&b, '.'); pb_u64(&b, centi);
	pb_char(&b, '\n');
	return (int)(sz - b.rem);
}

static int fill_stat(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	uint64_t tb = 0, ti = 0;
	for (int i = 0; i < smp_ncpus; i++)
		if (all_cpus[i]) { tb += all_cpus[i]->busy_ticks; ti += all_cpus[i]->idle_ticks; }
	(void)kernel_ticks;
	pb_str(&b, "cpu  ");
	pb_u64(&b, TICKS_USER(tb)); pb_str(&b, " 0 0 "); pb_u64(&b, TICKS_USER(ti)); pb_str(&b, " 0 0 0 0\n");
	for (int i = 0; i < smp_ncpus; i++) {
		pb_str(&b, "cpu"); pb_u64(&b, i); pb_str(&b, " ");
		pb_u64(&b, TICKS_USER(all_cpus[i] ? all_cpus[i]->busy_ticks : 0)); pb_str(&b, " 0 0 ");
		pb_u64(&b, TICKS_USER(all_cpus[i] ? all_cpus[i]->idle_ticks : 0)); pb_str(&b, " 0 0 0 0\n");
	}
	pb_str(&b, "intr 0\n");
	pb_str(&b, "ctxt 0\n");
	pb_str(&b, "btime 0\n");          /* boot epoch — vmstat/uptime read these */
	pb_str(&b, "processes "); pb_u64(&b, (uint64_t)task_count()); pb_char(&b, '\n');
	pb_str(&b, "procs_running "); pb_u64(&b, (uint64_t)sched_running_now()); pb_char(&b, '\n');
	pb_str(&b, "procs_blocked 0\n");
	return (int)(sz - b.rem);
}

static int fill_loadavg(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	for (int i = 0; i < 3; i++) {
		uint64_t v = load_avg[i] * 100 / 1024;
		pb_u64(&b, v / 100);
		pb_char(&b, '.');
		pb_char(&b, (char)('0' + v / 10 % 10));
		pb_char(&b, (char)('0' + v % 10));
		pb_char(&b, ' ');
	}
	pb_u64(&b, (uint64_t)sched_running_now());
	pb_char(&b, '/');
	pb_u64(&b, (uint64_t)task_count());
	pb_str(&b, " ");
	pb_u64(&b, (uint64_t)task_current()->pid);
	pb_char(&b, '\n');
	return (int)(sz - b.rem);
}

static void pb_u64_right(pb_t *b, uint64_t v, int width)
{
	int digits = 1;
	for (uint64_t t = v; t >= 10; t /= 10) digits++;
	while (width-- > digits) pb_char(b, ' ');
	pb_u64(b, v);
}

/* IRQ numbers are the arch's own (vectors on x86), labelled by handler symbol */
static int fill_interrupts(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	int cpus = smp_ncpus < 32 ? smp_ncpus : 32;
	pb_str(&b, "    ");
	for (int c = 0; c < cpus; c++) {
		pb_str(&b, "       CPU");
		pb_u64(&b, (uint64_t)c);
	}
	pb_char(&b, '\n');
	for (uint32_t irq = 0; irq < 128; irq++) {
		uint64_t handler = 0, total = 0, off = 0;
		for (int c = 0; c < cpus; c++) total += irq_stat(irq, c, &handler);
		if (!handler && !total) continue;
		pb_u64_right(&b, irq, 3);
		pb_char(&b, ':');
		for (int c = 0; c < cpus; c++) {
			uint64_t unused;
			pb_u64_right(&b, irq_stat(irq, c, &unused), 11);
		}
		const char *name = handler ? ksym_lookup(handler, &off) : NULL;
		pb_str(&b, "   ");
		pb_str(&b, name ? name : "unhandled");
		pb_char(&b, '\n');
	}
	return (int)(sz - b.rem);
}

/* ── per-process fill callbacks ──────────────────────────────── */

/* Returns a pinned process (proc_get); every caller must proc_put it. */
static process_t *find_proc(int pid)
{
	return proc_get((pid_t)pid);
}

/* Address-space size in bytes: sum of mapped VMA extents. With no per-page
 * present accounting this is our best VmSize/RSS proxy (Linux RSS counts
 * resident pages; here everything mapped is treated resident). */
static uint64_t proc_vm_bytes(process_t *p)
{
	uint64_t total = 0;
	if (!p->vm) return 0;
	for (vma_t *v = p->vm->vmas; v; v = v->next) total += v->size;
	return total;
}

/* Resident: what the space was charged for (pages faulted in or allocated up front),
 * less its page tables. Reserved but untouched ranges cost nothing and count nothing. */
static uint64_t proc_rss_bytes(process_t *p)
{
	if (!p->vm) return 0;
	long n = p->vm->mem_charged - p->vm->pt_charged;
	return n > 0 ? (uint64_t)n : 0;
}

/* "NStgid:" / "NSpid:" lines (Linux): the task's number in each pid namespace
 * from the reader's own down to the task's, tab-separated. A sandboxed task
 * (Chromium's renderer) shows two or more. */
static void pb_ns_ids(pb_t *b, const char *label, process_t *t)
{
	/* pidnrs[] runs from the initial namespace (0) to the task's own */
	pid_namespace_t *mine = current_pid_ns();
	int from = 0;
	for (int i = 0; i < t->pid_levels; i++)
		if (t->pidnrs[i].ns == mine) { from = i; break; }
	pb_str(b, label);
	if (t->pid_levels == 0) { pb_char(b, '\t'); pb_u64(b, (uint64_t)t->pid); }
	for (int i = from; i < t->pid_levels; i++) {
		pb_char(b, '\t');
		pb_u64(b, (uint64_t)t->pidnrs[i].nr);
	}
	pb_char(b, '\n');
}

static int fill_proc_status(char *buf, size_t sz, int pid)
{
	process_t *p = find_proc(pid);
	pb_t b = {buf, sz};
	if (!p) { pb_str(&b, "Name:\t(gone)\n"); return (int)(sz - b.rem); }
	pb_str(&b, "Name:\t"); pb_str(&b, p->name); pb_char(&b, '\n');
	const char *st = (p->state == PROC_SLEEPING) ? "S (sleeping)"
	               : (p->state == PROC_ZOMBIE || p->state == PROC_DEAD) ? "Z (zombie)"
	               : "R (running)";
	pb_str(&b, "State:\t"); pb_str(&b, st); pb_char(&b, '\n');
	pb_str(&b, "Tgid:\t"); pb_u64(&b, p->tgid ? p->tgid : p->pid); pb_char(&b, '\n');
	pb_str(&b, "Pid:\t");  pb_u64(&b, p->pid); pb_char(&b, '\n');
	pb_str(&b, "PPid:\t"); pb_u64(&b, p->ppid); pb_char(&b, '\n');
	process_t *leader = find_proc(p->tgid ? p->tgid : p->pid);
	pb_ns_ids(&b, "NStgid:", leader ? leader : p);
	pb_ns_ids(&b, "NSpid:", p);
	if (leader) proc_put(leader);
	pb_str(&b, "TracerPid:\t"); pb_u64(&b, (uint64_t)p->tracer); pb_char(&b, '\n');
	pb_str(&b, "PtraceStop:\t"); pb_u64(&b, (uint64_t)p->ptrace_stop); pb_char(&b, ' ');
	pb_u64(&b, (uint64_t)p->ptrace_stop_reported); pb_char(&b, ' ');
	pb_u64(&b, (uint64_t)p->ptrace_in_syscall); pb_char(&b, ' ');
	pb_u64(&b, (uint64_t)p->ptrace_opts); pb_char(&b, '\n');
	pb_str(&b, "Uid:\t");
	pb_u64(&b, p->uid);  pb_char(&b, '\t');
	pb_u64(&b, p->euid); pb_char(&b, '\t');
	pb_u64(&b, p->suid); pb_char(&b, '\t');
	pb_u64(&b, task_fsuid(p)); pb_char(&b, '\n');
	pb_str(&b, "Gid:\t");
	pb_u64(&b, p->gid);  pb_char(&b, '\t');
	pb_u64(&b, p->egid); pb_char(&b, '\t');
	pb_u64(&b, p->sgid); pb_char(&b, '\t');
	pb_u64(&b, task_fsgid(p)); pb_char(&b, '\n');
	pb_str(&b, "Groups:\t");
	for (int _i = 0; _i < p->ngroups; _i++) {
		if (_i) pb_char(&b, ' ');
		pb_u64(&b, p->groups[_i]);
	}
	pb_char(&b, '\n');
	uint64_t vm_kb = proc_vm_bytes(p) / 1024;
	uint64_t rss_kb = proc_rss_bytes(p) / 1024;
	pb_str(&b, "VmPeak:\t"); pb_u64(&b, vm_kb); pb_str(&b, " kB\n");
	pb_str(&b, "VmSize:\t"); pb_u64(&b, vm_kb); pb_str(&b, " kB\n");
	pb_str(&b, "VmHWM:\t");  pb_u64(&b, rss_kb); pb_str(&b, " kB\n");
	pb_str(&b, "VmRSS:\t");  pb_u64(&b, rss_kb); pb_str(&b, " kB\n");
	pb_str(&b, "Threads:\t"); pb_u64(&b, (uint64_t)proc_thread_count(p->tgid)); pb_char(&b, '\n');
	uint64_t sigign = 0, sigcgt = 0;
	for (int _i = 0; p->sighand && _i < NSIG; _i++) {
		void *h = (void *)p->sighand->actions[_i].sa_handler;
		if ((uintptr_t)h == 1) sigign |= (1UL << _i);
		else if (h)            sigcgt |= (1UL << _i);
	}
	pb_str(&b, "SigPnd:\t"); pb_hex16(&b, p->sig_pending); pb_char(&b, '\n');
	pb_str(&b, "SigBlk:\t"); pb_hex16(&b, p->sig_mask);    pb_char(&b, '\n');
	pb_str(&b, "SigIgn:\t"); pb_hex16(&b, sigign);         pb_char(&b, '\n');
	pb_str(&b, "SigCgt:\t"); pb_hex16(&b, sigcgt);         pb_char(&b, '\n');
	pb_str(&b, "CapInh:\t"); pb_hex16(&b, p->cap_inheritable); pb_char(&b, '\n');
	pb_str(&b, "CapPrm:\t"); pb_hex16(&b, p->cap_permitted);   pb_char(&b, '\n');
	pb_str(&b, "CapEff:\t"); pb_hex16(&b, p->cap_effective);   pb_char(&b, '\n');
	proc_put(p);
	return (int)(sz - b.rem);
}

static int fill_proc_stat(char *buf, size_t sz, int pid)
{
	process_t *p = find_proc(pid);
	pb_t b = {buf, sz};
	if (!p) { pb_u64(&b, (uint64_t)pid); pb_str(&b, " (gone) Z 0\n"); return (int)(sz - b.rem); }
	/* /proc/PID/stat: the fields ps/top actually read (state, pgrp, session,
	 * nice, num_threads, vsize, rss) carry real values; the rest stay 0. */
	char st = (p->state == PROC_SLEEPING) ? 'S'
	        : (p->state == PROC_ZOMBIE)   ? 'Z'
	        : (p->state == PROC_DEAD)     ? 'X' : 'R';
	int prio = (p->rt_priority > 0) ? -(1 + p->rt_priority) : 20;
	pb_u64(&b, p->pid); pb_str(&b, " ("); pb_str(&b, p->name); pb_str(&b, ") ");
	pb_char(&b, st); pb_char(&b, ' ');                          /* 3: state */
	pb_u64(&b, p->ppid);           pb_char(&b, ' ');            /* 4: ppid */
	pb_u64(&b, (uint64_t)p->pgrp); pb_char(&b, ' ');            /* 5: pgrp */
	pb_u64(&b, (uint64_t)p->sid);  pb_char(&b, ' ');            /* 6: session */
	pb_str(&b, "0 -1 0 0 0 0 0 ");                              /* 7-13: tty_nr tpgid flags flt counters */
	pb_u64(&b, TICKS_USER(p->utime_ticks));  pb_char(&b, ' ');              /* 14: utime  */
	pb_u64(&b, TICKS_USER(p->stime_ticks));  pb_char(&b, ' ');              /* 15: stime  */
	pb_u64(&b, TICKS_USER(p->cutime_ticks)); pb_char(&b, ' ');              /* 16: cutime */
	pb_u64(&b, TICKS_USER(p->cstime_ticks)); pb_char(&b, ' ');              /* 17: cstime */
	if (prio < 0) { pb_char(&b, '-'); pb_u64(&b, (uint64_t)(-prio)); }       else pb_u64(&b, (uint64_t)prio);
	pb_char(&b, ' ');                                           /* 18: priority */
	if (p->nice < 0) { pb_char(&b, '-'); pb_u64(&b, (uint64_t)(-p->nice)); } else pb_u64(&b, (uint64_t)p->nice);
	pb_char(&b, ' ');                                           /* 19: nice */
	pb_u64(&b, (uint64_t)proc_thread_count(p->tgid)); pb_char(&b, ' ');  /* 20: num_threads */
	uint64_t vm_bytes = proc_vm_bytes(p);
	pb_str(&b, "0 ");                                           /* 21: itrealvalue */
	pb_u64(&b, p->start_tick);   pb_char(&b, ' ');              /* 22: starttime (ticks since boot) */
	pb_u64(&b, vm_bytes); pb_char(&b, ' '); /* 23: vsize (bytes) */
	pb_u64(&b, proc_rss_bytes(p) / PAGE_SIZE); pb_char(&b, ' '); /* 24: rss (pages) */
	pb_str(&b, "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");  /* 25-52 */
	proc_put(p);
	return (int)(sz - b.rem);
}

/* /proc/PID/auxv — the auxiliary vector its image started with, binary. */
static int fill_proc_auxv(char *buf, size_t sz, int pid)
{
	if (!proc_may_access(pid)) return 0;
	process_t *p = find_proc(pid);
	if (!p) return 0;
	size_t n = p->saved_auxv_len;
	if (n > sz) n = sz;
	memcpy(buf, p->saved_auxv, n);
	proc_put(p);
	return (int)n;
}

/* /proc/PID/statm — size resident shared text lib data dt, all in pages. */
/* What a task is blocked in: "nr arg0 arg1 arg2", or "running" in user mode. */
static int fill_proc_syscall(char *buf, size_t sz, int pid)
{
	process_t *p = find_proc(pid);
	pb_t b = {buf, sz};
	process_t *me = task_current();
	if (!p) return 0;
	if (me->euid == 0 || me->euid == p->euid) {
		arch_context_t *c = p->uctx;
		if (!p->in_syscall || !c) {
			pb_str(&b, "running\n");
		} else {
			pb_u64(&b, (uint64_t)p->last_syscall);     pb_char(&b, ' ');
			pb_u64(&b, (uint64_t)CTX_SYSCALL_ARG0(c)); pb_char(&b, ' ');
			pb_u64(&b, (uint64_t)CTX_SYSCALL_ARG1(c)); pb_char(&b, ' ');
			pb_u64(&b, (uint64_t)CTX_SYSCALL_ARG2(c)); pb_char(&b, '\n');
		}
	}
	proc_put(p);
	return (int)(sz - b.rem);
}

static int fill_proc_statm(char *buf, size_t sz, int pid)
{
	process_t *p = find_proc(pid);
	pb_t b = {buf, sz};
	uint64_t vmpg = p ? proc_vm_bytes(p) / PAGE_SIZE : 0;
	uint64_t size = vmpg ? vmpg : (p && p->vm && p->vm->brk ? p->vm->brk / PAGE_SIZE : 64);
	/* resident: the pages this space was charged for, not what it reserved; a
	 * browser reserving terabytes for its sandbox is not using them */
	uint64_t res  = p ? proc_rss_bytes(p) / PAGE_SIZE : size;
	pb_u64(&b, size);                  pb_char(&b, ' ');   /* size     */
	pb_u64(&b, res);                   pb_str(&b, " 0 1 0 ");/* resident shared text lib */
	pb_u64(&b, size > 1 ? size - 1 : 0); pb_str(&b, " 0\n"); /* data dt */
	if (p) proc_put(p);
	return (int)(sz - b.rem);
}

static int fill_cmdline(char *buf, size_t sz)
{
	const char *cl = cmdline_str();
	size_t n = strlen(cl);
	if (n >= sz) n = sz - 1;
	memcpy(buf, cl, n);
	buf[n++] = '\n';
	return (int)n;
}

static int fill_proc_cmdline(char *buf, size_t sz, int pid)
{
	process_t *p = find_proc(pid);
	if (!p) return 0;
	pb_t b = {buf, sz};
	if (p->cmdline && p->cmdline_len) {
		for (uint32_t i = 0; i < p->cmdline_len; i++)
			pb_char(&b, p->cmdline[i]);
	} else {
		/* kernel threads and not-yet-exec'd tasks have no argv: Linux emits the
		 * comm in [brackets]; keep the bare comm so ps still labels them. */
		pb_str(&b, p->name);
		pb_char(&b, '\0');
	}
	proc_put(p);
	return (int)(sz - b.rem);
}

static int fill_proc_comm(char *buf, size_t sz, int pid)
{
	process_t *p = find_proc(pid);
	if (!p) return 0;
	pb_t b = {buf, sz};
	pb_str(&b, p->name);
	pb_char(&b, '\n');
	proc_put(p);
	return (int)(sz - b.rem);
}

static int fill_proc_filesystems(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	for (int i = 0; ; i++) {
		const char *name = vfs_fstype_name(i);
		if (!name) break;
		/* block-backed filesystems omit the "nodev" tag */
		static const char *const BLK[] = {"ext2", "ext4", "vfat", "fat", "msdos", "exfat", "ntfs", "iso9660", "iso", NULL};
		int blk = 0;
		for (int k = 0; BLK[k]; k++) blk |= !strcmp(name, BLK[k]);
		if (!blk) pb_str(&b, "nodev");
		pb_char(&b, '\t');
		pb_str(&b, name);
		pb_char(&b, '\n');
	}
	return (int)(sz - b.rem);
}

static int fill_proc_maps(char *buf, size_t sz, int pid)
{
	pb_t b = {buf, sz};
	if (!proc_may_access(pid)) return (int)(sz - b.rem);   /* ptrace gate; empty on denial */
	process_t *p = find_proc(pid);
	if (!p || !p->vm) return (int)(sz - b.rem);
	/* iterate VMAs */
	for (vma_t *v = p->vm->vmas; v; v = v->next) {
		/* start-end perms offset dev:ino path */
		uint64_t end = v->vaddr + v->size;
		/* print hex addr */
		char hex[17]; int hi;
		uint64_t tmp = v->vaddr;
		for (hi = 15; hi >= 0; hi--) { hex[hi] = "0123456789abcdef"[tmp & 0xf]; tmp >>= 4; }
		hex[16] = '\0'; pb_str(&b, hex); pb_char(&b, '-');
		tmp = end;
		for (hi = 15; hi >= 0; hi--) { hex[hi] = "0123456789abcdef"[tmp & 0xf]; tmp >>= 4; }
		pb_str(&b, hex); pb_char(&b, ' ');
		pb_char(&b, (v->flags & VMA_READ)  ? 'r' : '-');
		pb_char(&b, (v->flags & VMA_WRITE) ? 'w' : '-');
		pb_char(&b, (v->flags & VMA_EXEC)  ? 'x' : '-');
		pb_char(&b, 'p');
		pb_str(&b, " 00000000 00:00 0\n");
	}
	proc_put(p);
	return (int)(sz - b.rem);
}

/* ── procfs inode types ──────────────────────────────────────── */

/* priv for a virtual file — fill callback + pid */
typedef struct {
	int  pid;   /* -1 for top-level files */
	int  type;  /* proc file type index */
} procfs_inode_priv_t;

#define PROC_F_VERSION   0
#define PROC_F_CPUINFO   1
#define PROC_F_MEMINFO   2
#define PROC_F_MOUNTS    3
#define PROC_F_UPTIME    4
#define PROC_F_STAT      5
#define PROC_F_LOADAVG   6
#define PROC_F_CMDLINE   7
#define PROC_F_FILESYSTEMS 8
#define PROC_F_MODULES   9
#define PROC_F_SWAPS     15
#define PROC_F_VMSTAT    16
#define PROC_F_PARTITIONS 17
#define PROC_F_DISKSTATS 18
#define PROC_F_P_STATUS  10
#define PROC_F_P_STAT    11
#define PROC_F_P_CMDLINE 12
#define PROC_F_P_MAPS    13
#define PROC_F_P_COMM    14
#define PROC_F_P_STATM   19
#define PROC_F_P_SYSCALL 96
#define PROC_F_P_AUXV    97
#define PROC_F_P_MOUNTINFO 25
#define PROC_F_P_OOMADJ  98
#define PROC_F_P_OOMSCORE 99
#define PROC_F_NET_DEV   20
#define PROC_F_NET_TCP   21
#define PROC_F_NET_UDP   22
#define PROC_F_NET_ROUTE 23
#define PROC_F_NET_IF6   24
#define PROC_F_NET_UNIX  29

/* /proc/sys entries */
#define PROC_F_SYS_HZ            30
#define PROC_F_SYS_HOSTNAME      31
#define PROC_F_SYS_OSTYPE        26
#define PROC_F_SYS_DOMAINNAME    32
#define PROC_F_SYS_OVERCOMMIT    33
#define PROC_F_SYS_NGROUPS_MAX   34
#define PROC_F_SYS_PID_MAX       35
#define PROC_F_SYS_FILE_MAX      36
#define PROC_F_SYS_SOMAXCONN     37
#define PROC_F_SYS_RMEM_DEFAULT  38
#define PROC_F_SYS_WMEM_DEFAULT  39
#define PROC_F_SYS_RMEM_MAX      40
#define PROC_F_SYS_WMEM_MAX      41
#define PROC_F_SYS_NFILE         42
#define PROC_F_SYS_DENTRY        50
#define PROC_F_SYS_RANDOMIZE_VA  43
#define PROC_F_SYS_CORE_PATTERN  44
#define PROC_F_SYS_KPTR_RESTRICT  45
#define PROC_F_SYS_DMESG_RESTRICT 46
#define PROC_F_SYS_MAX_MAP_COUNT  47
#define PROC_F_SYS_SWAPPINESS     48
#define PROC_F_INTERRUPTS         49
#define PROC_F_ASESS_STATUS       90
#define PROC_F_ASESS_TASKS        91
#define PROC_F_ASESS_DENIED       92
#define PROC_F_HWDIAG             93
#define PROC_F_SYS_RANIX_DEADLINE 94

/* kernel.kptr_restrict: 0 = show kernel pointers, 1 = hide from non-root, 2 =
 * hide from all. Default 1. No raw kernel pointer reaches /proc today, so this
 * is the policy knob a %pK printer must consult once one is added.
 * kernel.dmesg_restrict: 1 (default) restricts /dev/kmsg reads to privileged. */
int kptr_restrict  = 1;
int dmesg_restrict = 1;

/* /proc/modules — monolithic kernel: no loadable modules (lsmod shows nothing). */
static int fill_modules(char *buf, size_t sz) { (void)buf; (void)sz; return 0; }

/* /proc/swaps — no swap areas configured; header only (swapon -s). */
static int fill_swaps(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
	return (int)(sz - b.rem);
}

/* /proc/partitions — registered block devices (lsblk/fdisk/df probe this).
 * #blocks is in 1 KiB units; size_blocks counts 512-byte sectors. */
static int fill_partitions(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "major minor  #blocks  name\n");
	int n = block_device_count();
	for (int i = 0; i < n; i++) {
		block_device_t *d = block_device_get(i);
		if (!d || !d->size_blocks) continue;   /* a pulled disk, a partition no longer there */
		pb_str(&b, "   8 "); pb_u64(&b, (uint64_t)i); pb_str(&b, " ");
		pb_u64(&b, d->size_blocks / 2); pb_str(&b, " ");
		pb_str(&b, d->name); pb_char(&b, '\n');
	}
	return (int)(sz - b.rem);
}

/* /proc/diskstats — one line per block device (iostat/df probe it). We don't
 * track per-device I/O counters yet, so the 11 stat fields are zero. */
static int fill_diskstats(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	int n = block_device_count();
	for (int i = 0; i < n; i++) {
		block_device_t *d = block_device_get(i);
		if (!d) continue;
		pb_str(&b, "   8 "); pb_u64(&b, (uint64_t)i); pb_str(&b, " ");
		pb_str(&b, d->name);
		pb_str(&b, " 0 0 0 0 0 0 0 0 0 0 0\n");
	}
	return (int)(sz - b.rem);
}

/* /proc/vmstat — a few real counters tools may read. */
static int fill_vmstat(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "nr_free_pages "); pb_u64(&b, buddy_free_pages()); pb_char(&b, '\n');
	pb_str(&b, "nr_anon_pages 0\nnr_mapped 0\npgfault 0\npgmajfault 0\n");
	return (int)(sz - b.rem);
}

static int fill_asess_status(uint64_t id, char *buf, size_t sz);
static int fill_asess_tasks(uint64_t id, char *buf, size_t sz);
static int fill_asess_denied(uint64_t id, char *buf, size_t sz);
static int fill_net_dev(char *buf, size_t sz);
static int fill_net_tcp(char *buf, size_t sz);
static int fill_net_udp(char *buf, size_t sz);
static int fill_net_route(char *buf, size_t sz);
static int fill_net_if_inet6(char *buf, size_t sz);
static int fill_net_unix(char *buf, size_t sz);

/* static file table for root dir */
static const struct {
	const char *name;
	int         type;
} root_files[] = {
	{ "version",  PROC_F_VERSION  },
	{ "cpuinfo",  PROC_F_CPUINFO  },
	{ "meminfo",  PROC_F_MEMINFO  },
	{ "mounts",   PROC_F_MOUNTS   },
	{ "uptime",   PROC_F_UPTIME   },
	{ "stat",     PROC_F_STAT     },
	{ "loadavg",  PROC_F_LOADAVG  },
	{ "cmdline",  PROC_F_CMDLINE  },
	{ "filesystems", PROC_F_FILESYSTEMS },
	{ "modules",  PROC_F_MODULES  },
	{ "swaps",    PROC_F_SWAPS    },
	{ "vmstat",   PROC_F_VMSTAT   },
	{ "partitions", PROC_F_PARTITIONS },
	{ "diskstats", PROC_F_DISKSTATS },
	{ "interrupts", PROC_F_INTERRUPTS },
	{ "hwdiag",   PROC_F_HWDIAG   },
};
#define ROOT_FILES_N ((int)(sizeof(root_files) / sizeof(root_files[0])))

/* static file table for per-pid subdirs */
static const struct {
	const char *name;
	int         type;
} pid_files[] = {
	{ "status",  PROC_F_P_STATUS  },
	{ "stat",    PROC_F_P_STAT    },
	{ "cmdline", PROC_F_P_CMDLINE },
	{ "maps",    PROC_F_P_MAPS    },
	{ "comm",    PROC_F_P_COMM    },
	{ "statm",   PROC_F_P_STATM   },
	{ "syscall", PROC_F_P_SYSCALL },
	{ "auxv",    PROC_F_P_AUXV    },
	{ "mountinfo", PROC_F_P_MOUNTINFO },
	{ "oom_score_adj", PROC_F_P_OOMADJ },
	{ "oom_score", PROC_F_P_OOMSCORE },
};
#define PID_FILES_N ((int)(sizeof(pid_files) / sizeof(pid_files[0])))

/* ── virtual file ops ────────────────────────────────────────── */

/* drivers/watchdog/wdog_core.c is x86_64 only: weak, so NULL elsewhere */
#pragma weak wdog_deadline_left
#pragma weak wdog_deadline_set
#define PROCFS_BUF_SIZE 4096

static ssize_t procfs_file_read(vfs_file_t *f, void *buf, size_t len)
{
	procfs_inode_priv_t *pr = f->inode->priv;
	if (!pr) return -1;

	uint64_t phys = (uint64_t)buddy_alloc(0);
	if (!phys) return -1;
	char *tmp = (char *)PHYS_TO_VIRT(phys);

	int n = 0;
	switch (pr->type) {
	case PROC_F_VERSION:  n = fill_version(tmp,  PROCFS_BUF_SIZE); break;
	case PROC_F_CPUINFO:  n = fill_cpuinfo(tmp,  PROCFS_BUF_SIZE); break;
	case PROC_F_MEMINFO:  n = fill_meminfo(tmp,  PROCFS_BUF_SIZE); break;
	case PROC_F_MOUNTS:   n = fill_mounts(tmp,   PROCFS_BUF_SIZE); break;
	case PROC_F_UPTIME:   n = fill_uptime(tmp,   PROCFS_BUF_SIZE); break;
	case PROC_F_STAT:     n = fill_stat(tmp,     PROCFS_BUF_SIZE); break;
	case PROC_F_LOADAVG:  n = fill_loadavg(tmp,  PROCFS_BUF_SIZE); break;
	case PROC_F_INTERRUPTS: n = fill_interrupts(tmp, PROCFS_BUF_SIZE); break;
	case PROC_F_HWDIAG:   n = hwdiag_fill(tmp,   PROCFS_BUF_SIZE); break;
	case PROC_F_ASESS_STATUS: n = fill_asess_status((uint64_t)pr->pid, tmp, PROCFS_BUF_SIZE); break;
	case PROC_F_ASESS_TASKS:  n = fill_asess_tasks((uint64_t)pr->pid, tmp, PROCFS_BUF_SIZE); break;
	case PROC_F_ASESS_DENIED: n = fill_asess_denied((uint64_t)pr->pid, tmp, PROCFS_BUF_SIZE); break;
	case PROC_F_CMDLINE:  n = fill_cmdline(tmp,  PROCFS_BUF_SIZE); break;
	case PROC_F_FILESYSTEMS: n = fill_proc_filesystems(tmp, PROCFS_BUF_SIZE); break;
	case PROC_F_MODULES:  n = fill_modules(tmp,  PROCFS_BUF_SIZE); break;
	case PROC_F_SWAPS:    n = fill_swaps(tmp,    PROCFS_BUF_SIZE); break;
	case PROC_F_VMSTAT:   n = fill_vmstat(tmp,   PROCFS_BUF_SIZE); break;
	case PROC_F_PARTITIONS: n = fill_partitions(tmp, PROCFS_BUF_SIZE); break;
	case PROC_F_DISKSTATS: n = fill_diskstats(tmp, PROCFS_BUF_SIZE); break;
	case PROC_F_P_STATUS: n = fill_proc_status(tmp, PROCFS_BUF_SIZE, pr->pid); break;
	case PROC_F_P_STAT:   n = fill_proc_stat(tmp,   PROCFS_BUF_SIZE, pr->pid); break;
	case PROC_F_P_STATM:  n = fill_proc_statm(tmp,  PROCFS_BUF_SIZE, pr->pid); break;
	case PROC_F_P_OOMADJ:
	case PROC_F_P_OOMSCORE: {
		process_t *p = find_proc(pr->pid);
		long v = 0;
		if (p) {
			if (pr->type == PROC_F_P_OOMADJ) v = p->oom_score_adj;
			else {                              /* 0..2000: share of memory plus the adjustment */
				uint64_t tot = buddy_total_pages() * 4096UL;
				v = tot ? (long)((uint64_t)proc_rss_bytes(p) * 1000 / tot) : 0;
				v += p->oom_score_adj;
				if (v < 0) v = 0;
				if (p->oom_score_adj <= -1000) v = 0;
			}
			proc_put(p);
		}
		char nb[24]; int k = 0, neg = v < 0; unsigned long u = neg ? (unsigned long)-v : (unsigned long)v;
		char rv[24]; int r = 0;
		do { rv[r++] = (char)('0' + u % 10); u /= 10; } while (u);
		if (neg) nb[k++] = '-';
		while (r) nb[k++] = rv[--r];
		nb[k++] = '\n';
		memcpy(tmp, nb, (size_t)k);
		n = k;
		break;
	}
	case PROC_F_P_SYSCALL: n = fill_proc_syscall(tmp, PROCFS_BUF_SIZE, pr->pid); break;
	case PROC_F_P_AUXV:   n = fill_proc_auxv(tmp, PROCFS_BUF_SIZE, pr->pid); break;
	case PROC_F_P_MOUNTINFO: n = (int)vfs_proc_mountinfo(tmp, PROCFS_BUF_SIZE); break;
	case PROC_F_P_CMDLINE:n = fill_proc_cmdline(tmp, PROCFS_BUF_SIZE, pr->pid); break;
	case PROC_F_P_MAPS:   n = fill_proc_maps(tmp,    PROCFS_BUF_SIZE, pr->pid); break;
	case PROC_F_P_COMM:   n = fill_proc_comm(tmp,    PROCFS_BUF_SIZE, pr->pid); break;
	case PROC_F_NET_DEV:   n = fill_net_dev(tmp,   PROCFS_BUF_SIZE); break;
	case PROC_F_NET_TCP:   n = fill_net_tcp(tmp,   PROCFS_BUF_SIZE); break;
	case PROC_F_NET_UDP:   n = fill_net_udp(tmp,   PROCFS_BUF_SIZE); break;
	case PROC_F_NET_ROUTE: n = fill_net_route(tmp, PROCFS_BUF_SIZE); break;
	case PROC_F_NET_IF6:   n = fill_net_if_inet6(tmp, PROCFS_BUF_SIZE); break;
	case PROC_F_NET_UNIX:  n = fill_net_unix(tmp,  PROCFS_BUF_SIZE); break;
	case PROC_F_SYS_HZ:           { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"100\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_OSTYPE:       { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b, task_current()->uname_linux ? "Linux" : "Ranix"); pb_char(&b,'\n'); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_HOSTNAME:     { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,current_uts()->nodename); pb_char(&b,'\n'); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_DOMAINNAME:   { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,current_uts()->domainname); pb_char(&b,'\n'); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_OVERCOMMIT:   { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"0\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_NGROUPS_MAX:  { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"65536\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_PID_MAX:      { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"32768\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_FILE_MAX:     { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"65536\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_SOMAXCONN:    { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"128\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_RMEM_DEFAULT: { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"212992\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_WMEM_DEFAULT: { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"212992\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_RMEM_MAX:     { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"134217728\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_WMEM_MAX:     { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"134217728\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_DENTRY:       { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_u64(&b,(uint64_t)vfs_dentry_count()); pb_str(&b,"\t0\t45\t0\t0\t0\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_NFILE:        { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"4096\t0\t65536\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_RANDOMIZE_VA: { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"0\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_CORE_PATTERN: { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,core_pattern); pb_char(&b,'\n'); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_KPTR_RESTRICT:  { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_u64(&b,(uint64_t)kptr_restrict);  pb_char(&b,'\n'); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_RANIX_DEADLINE: { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_u64(&b,(uint64_t)(wdog_deadline_left ? wdog_deadline_left() : 0)); pb_char(&b,'\n'); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_DMESG_RESTRICT: { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_u64(&b,(uint64_t)dmesg_restrict); pb_char(&b,'\n'); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_MAX_MAP_COUNT:  { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"65530\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	case PROC_F_SYS_SWAPPINESS:     { pb_t b = {tmp,PROCFS_BUF_SIZE}; pb_str(&b,"0\n"); n=(int)(PROCFS_BUF_SIZE-b.rem); } break;
	default: n = 0; break;
	}

	/* serve from pos */
	ssize_t ret = 0;
	if (f->pos < (off_t)n) {
		size_t avail = (size_t)(n - f->pos);
		if (len > avail) len = avail;
		if (copy_fop_to_user(buf, tmp + f->pos, len) != 0) { buddy_free(phys, 0); return -EFAULT; }
		f->pos += (off_t)len;
		ret = (ssize_t)len;
	}

	buddy_free(phys, 0);
	return ret;
}

static ssize_t procfs_sysctl_write(vfs_file_t *f, const void *buf, size_t len)
{
	procfs_inode_priv_t *pr = f->inode->priv;
	if (!pr) return -1;

	if (pr->type == PROC_F_P_OOMADJ) {
		char tmp[16];
		size_t n = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
		if (copy_fop_from_user(tmp, buf, n) != 0) return -EFAULT;
		tmp[n] = '\0';
		int neg = tmp[0] == '-', i = neg, v = 0;
		if (tmp[i] < '0' || tmp[i] > '9') return -EINVAL;
		for (; tmp[i] >= '0' && tmp[i] <= '9'; i++) v = v * 10 + (tmp[i] - '0');
		if (neg) v = -v;
		if (v < -1000 || v > 1000) return -EINVAL;
		process_t *p = find_proc(pr->pid);
		if (!p) return -ESRCH;
		process_t *me = task_current();
		/* anyone may make themselves a likelier victim; lowering takes root */
		int ok = (me->euid == 0) || ((me->euid == p->uid) && v >= p->oom_score_adj);
		if (ok) p->oom_score_adj = v;
		proc_put(p);
		return ok ? (ssize_t)len : -13;
	}

	if (pr->type == PROC_F_SYS_HOSTNAME) {
		if (len == 0 || len >= 65) return -EINVAL;
		char tmp[65];
		if (copy_fop_from_user(tmp, buf, len) != 0) return -EFAULT;
		size_t n = len;
		if (n > 0 && tmp[n-1] == '\n') n--;
		if (n >= 65) return -EINVAL;
		process_t *wr = task_current();
		if (!capable_task(wr, CAP_SYS_ADMIN) &&
		    !(cap_has(wr->cap_effective, CAP_SYS_ADMIN) && wr->uts_ns && wr->uts_ns != &init_uts_ns))
			return -EPERM;
		if (n > 0) {
			uts_namespace_t *ns = current_uts();
			memcpy(ns->nodename, tmp, n);
			ns->nodename[n] = '\0';
		}
		return (ssize_t)len;
	}
	if (pr->type == PROC_F_SYS_CORE_PATTERN) {
		if (!capable_task(task_current(), CAP_SYS_ADMIN)) return -EPERM;
		if (len == 0 || len >= 128) return -EINVAL;
		char tmp[128];
		if (copy_fop_from_user(tmp, buf, len) != 0) return -EFAULT;
		size_t n = len;
		if (n > 0 && tmp[n-1] == '\n') n--;
		memcpy(core_pattern, tmp, n);
		core_pattern[n] = '\0';
		return (ssize_t)len;
	}
	if (pr->type == PROC_F_SYS_RANIX_DEADLINE) {
		if (!capable_task(task_current(), CAP_SYS_ADMIN)) return -EPERM;
		char tmp[16];
		if (len == 0 || len >= sizeof(tmp)) return -EINVAL;
		if (copy_fop_from_user(tmp, buf, len) != 0) return -EFAULT;
		size_t n = len;
		if (tmp[n-1] == '\n') n--;
		if (n == 0 || n > 9) return -EINVAL;
		unsigned v = 0;
		for (size_t i = 0; i < n; i++) {
			if (tmp[i] < '0' || tmp[i] > '9') return -EINVAL;
			v = v * 10 + (unsigned)(tmp[i] - '0');
		}
		if (!wdog_deadline_set) return -ENOSYS;   /* no deadline on this arch */
		int r = wdog_deadline_set(v);
		return r ? r : (ssize_t)len;
	}
	if (pr->type == PROC_F_SYS_KPTR_RESTRICT || pr->type == PROC_F_SYS_DMESG_RESTRICT) {
		process_t *c = task_current();
		if (c && c->euid != 0 && !capable_task(c, CAP_SYS_ADMIN)) return -EPERM;
		char tmp[16];
		size_t n = len < sizeof(tmp) - 1 ? len : sizeof(tmp) - 1;
		if (copy_fop_from_user(tmp, buf, n) != 0) return -EFAULT;
		tmp[n] = '\0';
		int v = (tmp[0] >= '0' && tmp[0] <= '9') ? tmp[0] - '0' : 0;
		if (pr->type == PROC_F_SYS_KPTR_RESTRICT) { if (v > 2) v = 2; kptr_restrict = v; }
		else                                       dmesg_restrict = v ? 1 : 0;
		return (ssize_t)len;
	}
	return -1;
}

static vfs_fops_t procfs_fops = {
	.read  = procfs_file_read,
	.write = procfs_sysctl_write,
};

static vfs_fops_t procfs_dir_fops = { 0 };

/* ── inode allocation ────────────────────────────────────────── */

static uint64_t   proc_ino_next = 0x8000;
static spinlock_t proc_ino_lock = SPINLOCK_INIT;

static uint64_t proc_ino_alloc(void)
{
	uint64_t f = spin_lock_irq(&proc_ino_lock);
	uint64_t n = proc_ino_next++;
	spin_unlock_irq(&proc_ino_lock, f);
	return n;
}

/* ── /proc/<pid>/mem — cross-process memory R/W (gdb) ────────────────────────
 * The file offset is the virtual address in the target's address space. */

static ssize_t procfs_mem_rw(vfs_file_t *f, void *ubuf, size_t len, int write)
{
	procfs_inode_priv_t *pr = (procfs_inode_priv_t *)f->inode->priv;
	if (!proc_may_access(pr->pid)) return -EPERM;   /* ptrace gate */
	vm_space_t *vm = proc_get_vm(pr->pid);   /* pinned; vm_destroy to release */
	if (!vm) return -ESRCH;
	uint64_t vaddr = (uint64_t)f->pos;
	char tmp[256];
	size_t done = 0;
	while (done < len) {
		size_t n = len - done; if (n > sizeof(tmp)) n = sizeof(tmp);
		size_t got;
		if (write) {
			if (copy_from_user(tmp, (char *)ubuf + done, n) != 0) break;
			got = vm_access_remote(vm, vaddr + done, tmp, n, 1);
		} else {
			got = vm_access_remote(vm, vaddr + done, tmp, n, 0);
			if (got && copy_to_user((char *)ubuf + done, tmp, got) != 0) break;
		}
		done += got;
		if (got < n) break;
	}
	f->pos += (long)done;
	vm_destroy(vm);   /* drop our pin */
	return done ? (ssize_t)done : (len ? -5 : 0);   /* EIO: nothing mapped there */
}

static ssize_t procfs_mem_read(vfs_file_t *f, void *buf, size_t len)
{ return procfs_mem_rw(f, buf, len, 0); }
static ssize_t procfs_mem_write(vfs_file_t *f, const void *buf, size_t len)
{ return procfs_mem_rw(f, (void *)buf, len, 1); }

static vfs_fops_t procfs_mem_fops = { .read = procfs_mem_read, .write = procfs_mem_write };

static vfs_inode_t *procfs_mem_inode(int pid)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino = proc_ino_alloc();
	in->mode = S_IFREG | 0600;
	in->nlink = 1; in->refcount = 1;
	in->fops = &procfs_mem_fops;
	procfs_inode_priv_t *pr = kmalloc(sizeof(*pr));
	if (!pr) { kfree(in); return NULL; }
	memset(pr, 0, sizeof(*pr));
	pr->pid = pid;
	in->priv = pr;
	return in;
}

static vfs_inode_t *procfs_inode_file(int pid, int type)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino     = proc_ino_alloc();
	in->mode    = S_IFREG | (type == PROC_F_P_OOMADJ ? 0644 : 0444);
	in->nlink   = 1;
	in->refcount= 1;
	in->fops    = &procfs_fops;

	procfs_inode_priv_t *pr = kmalloc(sizeof(*pr));
	if (!pr) { kfree(in); return NULL; }
	memset(pr, 0, sizeof(*pr));
	pr->pid  = pid;
	pr->type = type;
	in->priv = pr;
	return in;
}

/* ── /proc/<pid>/fd/<n> symlink ──────────────────────────────── */

typedef struct { int pid; int fd_n; } procfs_fdlink_priv_t;

static int procfs_fdlink_readlink(vfs_inode_t *in, char *buf, size_t sz)
{
	procfs_fdlink_priv_t *pr = in->priv;
	if (!pr) return -1;
	int self = (pr->pid == 0);
	fd_table_t *fds = self ? task_current()->fds : proc_get_fds((pid_t)pr->pid);
	if (!fds) return -1;
	int ret = -1;
	vfs_file_t *f = fget(fds, pr->fd_n);   /* pin the file across the deref */
	if (f) {
		if (f->inode) {
			const char *path = f->path[0] ? f->path : "anon";
			size_t n = strlen(path);
			if (n + 1 <= sz) { memcpy(buf, path, n + 1); ret = 0; }
		}
		fput(f);
	}
	if (!self) fd_table_put(fds);
	return ret;
}

/* The open file itself, pinned, for a walk that cannot follow the text */
static vfs_file_t *procfs_fdlink_file(vfs_inode_t *in)
{
	procfs_fdlink_priv_t *pr = in->priv;
	if (!pr) return NULL;
	int self = (pr->pid == 0);
	fd_table_t *fds = self ? task_current()->fds : proc_get_fds((pid_t)pr->pid);
	if (!fds) return NULL;
	vfs_file_t *f = fget(fds, pr->fd_n);
	if (!self) fd_table_put(fds);
	return f;
}

static vfs_iops_t procfs_fdlink_iops = {
	.readlink = procfs_fdlink_readlink, .link_file = procfs_fdlink_file,
};

static vfs_inode_t *procfs_fdlink_inode(int pid, int fd_n)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = proc_ino_alloc();
	in->mode     = S_IFLNK | 0777;
	in->nlink    = 1;
	in->refcount = 1;
	in->iops     = &procfs_fdlink_iops;
	procfs_fdlink_priv_t *pr = kmalloc(sizeof(*pr));
	if (!pr) { kfree(in); return NULL; }
	pr->pid = pid; pr->fd_n = fd_n;
	in->priv = pr;
	return in;
}

/* ── /proc/<pid>/fd directory ────────────────────────────────── */

typedef struct { int pid; } procfs_fddir_priv_t;

static int procfs_fds_visible(int pid);

/* fd/N of a task the reader may not see is not there: the link names the
 * file, and walking it reaches the file's attributes (walk_fdlink) */
static vfs_inode_t *procfs_fddir_lookup(vfs_inode_t *dir, const char *name)
{
	procfs_fddir_priv_t *pp = dir->priv;
	int pid = pp ? pp->pid : 0;
	int fd_n = 0; const char *s = name;
	if (!*s || !procfs_fds_visible(pid)) return NULL;
	while (*s >= '0' && *s <= '9') { fd_n = fd_n * 10 + (*s - '0'); s++; }
	if (*s) return NULL;
	int self = (pid == 0);
	fd_table_t *fds = self ? task_current()->fds : proc_get_fds((pid_t)pid);
	if (!fds) return NULL;
	vfs_inode_t *ret = fd_get(fds, fd_n) ? procfs_fdlink_inode(pid, fd_n) : NULL;
	if (!self) fd_table_put(fds);
	return ret;
}

/* the fd table /proc/<pid>/fd and fdinfo list: pid 0 is the reader itself.
 * Release with procfs_fds_put. */
static fd_table_t *procfs_fds_get(int pid)
{
	return pid == 0 ? task_current()->fds : proc_get_fds((pid_t)pid);
}

static void procfs_fds_put(int pid, fd_table_t *fds)
{
	if (pid != 0) fd_table_put(fds);
}

/* the number of the idx-th open fd, or -1 */
static int procfs_fd_nth(fd_table_t *fds, uint32_t idx)
{
	uint32_t found = 0;
	for (int i = 0; i < fds->cap; i++) {
		if (!fd_get(fds, i)) continue;
		if (found++ == idx) return i;
	}
	return -1;
}

/* one directory entry named by the number v */
static void procfs_num_dirent(vfs_dirent_t *out, int v, uint32_t idx, uint32_t mode)
{
	char rev[12]; int ri = 0, ti = 0;
	do { rev[ri++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (ri) out->name[ti++] = rev[--ri];
	out->name[ti] = '\0';
	out->ino      = proc_ino_next;
	out->mode     = mode;
	out->next_off = idx + 1;
}

static int procfs_fddir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	procfs_fddir_priv_t *pp = dir->priv;
	int pid = pp ? pp->pid : 0;
	fd_table_t *fds = procfs_fds_get(pid);
	if (!fds) return -1;
	int i = procfs_fd_nth(fds, idx);
	if (i >= 0) procfs_num_dirent(out, i, idx, S_IFLNK | 0777);
	procfs_fds_put(pid, fds);
	return i >= 0 ? 0 : -1;
}

static vfs_iops_t procfs_fddir_iops = {
	.lookup  = procfs_fddir_lookup,
	.readdir = procfs_fddir_readdir,
};

static vfs_inode_t *procfs_fddir_inode(int pid)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = proc_ino_alloc();
	in->mode     = S_IFDIR | 0555;
	in->nlink    = 2;
	in->refcount = 1;
	in->iops     = &procfs_fddir_iops;
	in->fops     = &procfs_dir_fops;
	procfs_fddir_priv_t *pp = kmalloc(sizeof(*pp));
	if (!pp) { kfree(in); return NULL; }
	pp->pid = pid;
	in->priv = pp;
	return in;
}

/* ── /proc/<pid>/fdinfo: per open fd, its offset and open flags ──
 * Linux format ("pos:\t0\nflags:\t02\nmnt_id:\t0\nino:\t5\n"). The task's
 * own, as on Linux: the directory is 0500 and each file 0400 of its euid, and
 * a read by anyone else without CAP_SYS_PTRACE is refused. Chromium's sandbox
 * chroots into /proc/self/fdinfo/: a directory nothing can be made in. */

typedef struct { int pid; int fd_n; } procfs_fdinfo_priv_t;

/* may the reader see task pid's open files? */
static int procfs_fds_visible(int pid)
{
	process_t *cur = task_current();
	if (pid == 0) return 1;
	process_t *t = proc_get((pid_t)pid);
	if (!t) return 0;
	int ok = t->tgid == cur->tgid || t->euid == cur->euid ||
	         capable_task((struct process *)cur, CAP_SYS_PTRACE);
	proc_put(t);
	return ok;
}

/* the inode's owner: the task's euid/egid */
static void procfs_owner(vfs_inode_t *in, int pid)
{
	process_t *t = pid ? proc_get((pid_t)pid) : task_current();
	if (!t) return;
	in->uid = t->euid; in->gid = t->egid;
	if (pid) proc_put(t);
}

static int procfs_octal(char *b, unsigned v)
{
	char rev[12]; int r = 0, n = 0;
	b[n++] = '0';
	do { rev[r++] = (char)('0' + (v & 7)); v >>= 3; } while (v);
	while (r) b[n++] = rev[--r];
	return n;
}

static int procfs_decimal(char *b, uint64_t v)
{
	char rev[24]; int r = 0, n = 0;
	do { rev[r++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v);
	while (r) b[n++] = rev[--r];
	return n;
}

static int procfs_cat(char *b, const char *s)
{
	int n = (int)strlen(s);
	memcpy(b, s, (size_t)n);
	return n;
}

static int procfs_fdinfo_format(int pid, int fd_n, char *b)
{
	fd_table_t *fds = procfs_fds_get(pid);
	if (!fds) return -1;
	vfs_file_t *f = fget(fds, fd_n);   /* pin the file across the deref */
	unsigned flags = f ? (unsigned)f->flags : 0;
	if (f && fd_is_cloexec(fds, fd_n)) flags |= 02000000;   /* O_CLOEXEC, as Linux shows it */
	procfs_fds_put(pid, fds);
	if (!f) return -1;
	int n = 0;
	n += procfs_cat(b + n, "pos:\t");
	n += procfs_decimal(b + n, (uint64_t)(f->pos < 0 ? 0 : f->pos));
	n += procfs_cat(b + n, "\nflags:\t");
	n += procfs_octal(b + n, flags);
	n += procfs_cat(b + n, "\nmnt_id:\t0\nino:\t");
	n += procfs_decimal(b + n, f->inode ? (uint64_t)f->inode->ino : 0);
	b[n++] = '\n';
	fput(f);
	return n;
}

static ssize_t procfs_fdinfo_read(vfs_file_t *f, void *buf, size_t len)
{
	procfs_fdinfo_priv_t *pr = f->inode->priv;
	if (!procfs_fds_visible(pr->pid)) return -EACCES;
	char tmp[96];
	int n = procfs_fdinfo_format(pr->pid, pr->fd_n, tmp);
	if (n < 0) return -ENOENT;   /* closed since the lookup */
	if (f->pos >= (off_t)n) return 0;
	size_t avail = (size_t)(n - f->pos);
	if (len > avail) len = avail;
	if (copy_fop_to_user(buf, tmp + f->pos, len) != 0) return -EFAULT;
	f->pos += (off_t)len;
	return (ssize_t)len;
}

static vfs_fops_t procfs_fdinfo_fops = { .read = procfs_fdinfo_read };

static vfs_inode_t *procfs_fdinfo_inode(int pid, int fd_n)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = proc_ino_alloc();
	in->mode     = S_IFREG | 0400;
	in->nlink    = 1;
	in->refcount = 1;
	in->fops     = &procfs_fdinfo_fops;
	procfs_owner(in, pid);
	procfs_fdinfo_priv_t *pr = kmalloc(sizeof(*pr));
	if (!pr) { kfree(in); return NULL; }
	pr->pid = pid; pr->fd_n = fd_n;
	in->priv = pr;
	return in;
}

static vfs_inode_t *procfs_fdinfodir_lookup(vfs_inode_t *dir, const char *name)
{
	procfs_fddir_priv_t *pp = dir->priv;
	int pid = pp ? pp->pid : 0;
	int fd_n = 0; const char *s = name;
	if (!*s || !procfs_fds_visible(pid)) return NULL;
	while (*s >= '0' && *s <= '9') { fd_n = fd_n * 10 + (*s - '0'); s++; }
	if (*s) return NULL;
	fd_table_t *fds = procfs_fds_get(pid);
	if (!fds) return NULL;
	vfs_inode_t *ret = fd_get(fds, fd_n) ? procfs_fdinfo_inode(pid, fd_n) : NULL;
	procfs_fds_put(pid, fds);
	return ret;
}

static int procfs_fdinfodir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	procfs_fddir_priv_t *pp = dir->priv;
	int pid = pp ? pp->pid : 0;
	if (!procfs_fds_visible(pid)) return -1;
	fd_table_t *fds = procfs_fds_get(pid);
	if (!fds) return -1;
	int i = procfs_fd_nth(fds, idx);
	if (i >= 0) procfs_num_dirent(out, i, idx, S_IFREG | 0400);
	procfs_fds_put(pid, fds);
	return i >= 0 ? 0 : -1;
}

static vfs_iops_t procfs_fdinfodir_iops = {
	.lookup  = procfs_fdinfodir_lookup,
	.readdir = procfs_fdinfodir_readdir,
};

static vfs_inode_t *procfs_fdinfodir_inode(int pid)
{
	vfs_inode_t *in = procfs_fddir_inode(pid);
	if (!in) return NULL;
	in->mode = S_IFDIR | 0500;
	in->iops = &procfs_fdinfodir_iops;
	procfs_owner(in, pid);
	return in;
}

/* ── pid-dir iops ────────────────────────────────────────────── */

typedef struct {
	int pid;
} procfs_piddir_priv_t;

/* ── /proc/<pid>/{exe,cwd,root} symlinks ─────────────────────── */

enum { PIDLINK_EXE = 0, PIDLINK_CWD, PIDLINK_ROOT };
static const char *pid_links[] = { "exe", "cwd", "root" };
#define PID_LINKS_N ((int)(sizeof(pid_links) / sizeof(pid_links[0])))

typedef struct { int pid; int kind; } procfs_pidlink_priv_t;

static int procfs_pidlink_readlink(vfs_inode_t *in, char *buf, size_t sz)
{
	procfs_pidlink_priv_t *pr = in->priv;
	if (!pr) return -1;
	process_t *p = pr->pid == 0 ? task_current() : proc_get((pid_t)pr->pid);
	if (!p) return -1;
	const char *src;
	fs_struct_t *fs = NULL;
	fs_path_t *cwd = NULL;
	switch (pr->kind) {
	case PIDLINK_CWD:
		fs = pr->pid ? fs_of_task((struct process *)p) : NULL;   /* held against its exit */
		cwd = fs_cwd_get(pr->pid ? fs : p->fs);
		src = cwd->s;
		break;
	case PIDLINK_ROOT: src = "/"; break;
	default:           src = p->exe_path[0] ? p->exe_path : p->name; break;
	}
	char path[256];
	size_t n = strlen(src);
	if (n >= sizeof(path)) n = sizeof(path) - 1;
	memcpy(path, src, n); path[n] = '\0';
	fs_path_put(cwd);
	fs_put(fs);
	if (pr->pid) proc_put(p);
	if (n + 1 > sz) return -1;
	memcpy(buf, path, n + 1);
	return 0;
}

static vfs_iops_t procfs_pidlink_iops = { .readlink = procfs_pidlink_readlink };

static vfs_inode_t *procfs_pidlink_inode(int pid, int kind)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = proc_ino_alloc();
	in->mode     = S_IFLNK | 0777;
	in->nlink    = 1;
	in->refcount = 1;
	in->iops     = &procfs_pidlink_iops;
	procfs_pidlink_priv_t *pr = kmalloc(sizeof(*pr));
	if (!pr) { kfree(in); return NULL; }
	pr->pid = pid; pr->kind = kind;
	in->priv = pr;
	return in;
}

/* ── /proc/<pid>/ns/{uts,ipc} — namespace identity ("<kind>:[<inum>]") ── */

enum { NS_UTS = 0, NS_IPC, NS_TIME, NS_MNT, NS_PID, NS_USER, NS_NET, NS_CGROUP };
static const char *ns_kinds[] = { "uts", "ipc", "time", "mnt", "pid", "user", "net", "cgroup" };
#define NS_KINDS_N ((int)(sizeof(ns_kinds) / sizeof(ns_kinds[0])))

typedef struct { int pid; int kind; } procfs_nslink_priv_t;

/* Identity of pid's namespace of this kind (pid 0 = caller); 0 if gone. */
static unsigned long nslink_inum(int pid, int kind, const char **prefix_out)
{
	procfs_nslink_priv_t tmp = { pid, kind };
	procfs_nslink_priv_t *pp = &tmp;

	/* Pin the target's ns of this kind, snapshot its inum, then drop the pin —
	 * so a concurrent exit can never free the ns out from under the read. */
	const char *prefix;
	unsigned long inum;
	if (pp->pid == 0) {
		process_t *p = task_current();
		switch (pp->kind) {
		case NS_IPC:    prefix = "ipc:[";    inum = (p->ipc_ns ? p->ipc_ns : &init_ipc_ns)->inum; break;
		case NS_TIME:   prefix = "time:[";   inum = (p->time_ns ? p->time_ns : &init_time_ns)->inum; break;
		case NS_MNT:    prefix = "mnt:[";    inum = (p->mnt_ns ? p->mnt_ns : &init_mnt_ns)->inum; break;
		case NS_PID:    prefix = "pid:[";    inum = (p->pid_ns ? p->pid_ns : &init_pid_ns)->inum; break;
		case NS_USER:   prefix = "user:[";   inum = (p->user_ns ? p->user_ns : &init_user_ns)->inum; break;
		case NS_NET:    prefix = "net:[";    inum = (p->net_ns ? p->net_ns : &init_net_ns)->inum; break;
		case NS_CGROUP: prefix = "cgroup:["; inum = cgroup_ns_inum(p->cgroup_ns); break;
		default:        prefix = "uts:[";    inum = (p->uts_ns ? p->uts_ns : &init_uts_ns)->inum; break;
		}
	} else {
		void *ns = proc_get_ns((pid_t)pp->pid, pp->kind);
		if (!ns) return 0;
		switch (pp->kind) {
		case NS_IPC:    prefix = "ipc:[";    inum = ((ipc_namespace_t *)ns)->inum;  ipc_ns_put(ns);  break;
		case NS_TIME:   prefix = "time:[";   inum = ((time_namespace_t *)ns)->inum; time_ns_put(ns); break;
		case NS_MNT:    prefix = "mnt:[";    inum = ((mnt_namespace_t *)ns)->inum;  mnt_ns_put(ns);  break;
		case NS_PID:    prefix = "pid:[";    inum = ((pid_namespace_t *)ns)->inum;  pid_ns_put(ns);  break;
		case NS_USER:   prefix = "user:[";   inum = ((user_namespace_t *)ns)->inum; user_ns_put(ns); break;
		case NS_NET:    prefix = "net:[";    inum = ((net_namespace_t *)ns)->inum;  net_ns_put(ns);  break;
		case NS_CGROUP: prefix = "cgroup:["; inum = cgroup_ns_inum(ns);             cgroup_ns_put(ns); break;
		default:        prefix = "uts:[";    inum = ((uts_namespace_t *)ns)->inum;  uts_ns_put(ns);  break;
		}
	}
	if (prefix_out) *prefix_out = prefix;
	return inum;
}

static int procfs_nslink_readlink(vfs_inode_t *in, char *buf, size_t sz)
{
	procfs_nslink_priv_t *pp = in->priv;
	if (!pp) return -1;
	const char *prefix = "";
	unsigned long inum = nslink_inum(pp->pid, pp->kind, &prefix);
	if (!*prefix) return -1;

	char tmp[40]; int i = 0;
	for (const char *s = prefix; *s; s++) tmp[i++] = *s;
	char rev[20]; int r = 0;
	if (inum == 0) rev[r++] = '0';
	while (inum) { rev[r++] = '0' + (int)(inum % 10); inum /= 10; }
	while (r) tmp[i++] = rev[--r];
	tmp[i++] = ']';
	tmp[i] = '\0';
	if ((size_t)(i + 1) > sz) return -1;
	memcpy(buf, tmp, (size_t)i + 1);
	return 0;
}

static vfs_iops_t procfs_nslink_iops = { .readlink = procfs_nslink_readlink };

/*
 * Opening the ns link captures the target's namespace and pins it for the
 * life of the fd, so setns(2) can rejoin exactly that namespace even after
 * the target process unshares or exits. Separate fops per kind let setns
 * tell a UTS fd from an IPC one.
 */
static int procfs_utsns_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	procfs_nslink_priv_t *pp = in->priv;
	int pid = pp ? pp->pid : 0;
	f->priv = pid == 0 ? uts_ns_get(task_current()->uts_ns)
	                   : proc_get_ns((pid_t)pid, PROC_NS_UTS);
	if (!f->priv) return -1;
	return 0;
}

static int procfs_utsns_close(vfs_file_t *f)
{
	uts_ns_put((uts_namespace_t *)f->priv);
	f->priv = NULL;
	return 0;
}

static vfs_fops_t procfs_utsns_fops = {
	.open  = procfs_utsns_open,
	.close = procfs_utsns_close,
};

static int procfs_ipcns_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	procfs_nslink_priv_t *pp = in->priv;
	int pid = pp ? pp->pid : 0;
	f->priv = pid == 0 ? ipc_ns_get(task_current()->ipc_ns)
	                   : proc_get_ns((pid_t)pid, PROC_NS_IPC);
	if (!f->priv) return -1;
	return 0;
}

static int procfs_ipcns_close(vfs_file_t *f)
{
	ipc_ns_put((ipc_namespace_t *)f->priv);
	f->priv = NULL;
	return 0;
}

static vfs_fops_t procfs_ipcns_fops = {
	.open  = procfs_ipcns_open,
	.close = procfs_ipcns_close,
};

static int procfs_timens_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	procfs_nslink_priv_t *pp = in->priv;
	int pid = pp ? pp->pid : 0;
	f->priv = pid == 0 ? time_ns_get(task_current()->time_ns)
	                   : proc_get_ns((pid_t)pid, PROC_NS_TIME);
	if (!f->priv) return -1;
	return 0;
}

static int procfs_timens_close(vfs_file_t *f)
{
	time_ns_put((time_namespace_t *)f->priv);
	f->priv = NULL;
	return 0;
}

static vfs_fops_t procfs_timens_fops = {
	.open  = procfs_timens_open,
	.close = procfs_timens_close,
};

static int procfs_mntns_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	procfs_nslink_priv_t *pp = in->priv;
	int pid = pp ? pp->pid : 0;
	f->priv = pid == 0 ? mnt_ns_get(task_current()->mnt_ns)
	                   : proc_get_ns((pid_t)pid, PROC_NS_MNT);
	if (!f->priv) return -1;
	return 0;
}

static int procfs_mntns_close(vfs_file_t *f)
{
	mnt_ns_put((mnt_namespace_t *)f->priv);
	f->priv = NULL;
	return 0;
}

static vfs_fops_t procfs_mntns_fops = {
	.open  = procfs_mntns_open,
	.close = procfs_mntns_close,
};

static int procfs_pidns_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	procfs_nslink_priv_t *pp = in->priv;
	int pid = pp ? pp->pid : 0;
	f->priv = pid == 0 ? pid_ns_get(task_current()->pid_ns)
	                   : proc_get_ns((pid_t)pid, PROC_NS_PID);
	if (!f->priv) return -1;
	return 0;
}

static int procfs_pidns_close(vfs_file_t *f)
{
	pid_ns_put((pid_namespace_t *)f->priv);
	f->priv = NULL;
	return 0;
}

static vfs_fops_t procfs_pidns_fops = {
	.open  = procfs_pidns_open,
	.close = procfs_pidns_close,
};

static int procfs_userns_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	procfs_nslink_priv_t *pp = in->priv;
	int pid = pp ? pp->pid : 0;
	f->priv = pid == 0 ? user_ns_get(task_current()->user_ns)
	                   : proc_get_ns((pid_t)pid, PROC_NS_USER);
	if (!f->priv) return -1;
	return 0;
}

static int procfs_userns_close(vfs_file_t *f)
{
	user_ns_put((user_namespace_t *)f->priv);
	f->priv = NULL;
	return 0;
}

static vfs_fops_t procfs_userns_fops = {
	.open  = procfs_userns_open,
	.close = procfs_userns_close,
};

static int procfs_netns_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	procfs_nslink_priv_t *pp = in->priv;
	int pid = pp ? pp->pid : 0;
	f->priv = pid == 0 ? net_ns_get(task_current()->net_ns)
	                   : proc_get_ns((pid_t)pid, PROC_NS_NET);
	if (!f->priv) return -1;
	return 0;
}

static int procfs_netns_close(vfs_file_t *f)
{
	net_ns_put((net_namespace_t *)f->priv);
	f->priv = NULL;
	return 0;
}

static vfs_fops_t procfs_netns_fops = {
	.open  = procfs_netns_open,
	.close = procfs_netns_close,
};

static int procfs_cgroupns_open(vfs_inode_t *in, vfs_file_t *f, int flags)
{
	(void)flags;
	procfs_nslink_priv_t *pp = in->priv;
	int pid = pp ? pp->pid : 0;
	f->priv = pid == 0 ? cgroup_ns_get(task_current()->cgroup_ns)
	                   : proc_get_ns((pid_t)pid, PROC_NS_CGROUP);
	if (!f->priv) return -1;
	return 0;
}

static int procfs_cgroupns_close(vfs_file_t *f)
{
	cgroup_ns_put((struct cgroup_namespace *)f->priv);
	f->priv = NULL;
	return 0;
}

static vfs_fops_t procfs_cgroupns_fops = {
	.open  = procfs_cgroupns_open,
	.close = procfs_cgroupns_close,
};

/* setns(2) helpers: the namespace an open ns fd refers to, or NULL */
uts_namespace_t *nsfile_to_uts(vfs_file_t *f)
{
	if (!f || !f->inode || f->inode->fops != &procfs_utsns_fops) return NULL;
	return (uts_namespace_t *)f->priv;
}

ipc_namespace_t *nsfile_to_ipc(vfs_file_t *f)
{
	if (!f || !f->inode || f->inode->fops != &procfs_ipcns_fops) return NULL;
	return (ipc_namespace_t *)f->priv;
}

time_namespace_t *nsfile_to_time(vfs_file_t *f)
{
	if (!f || !f->inode || f->inode->fops != &procfs_timens_fops) return NULL;
	return (time_namespace_t *)f->priv;
}

mnt_namespace_t *nsfile_to_mnt(vfs_file_t *f)
{
	if (!f || !f->inode || f->inode->fops != &procfs_mntns_fops) return NULL;
	return (mnt_namespace_t *)f->priv;
}

pid_namespace_t *nsfile_to_pid(vfs_file_t *f)
{
	if (!f || !f->inode || f->inode->fops != &procfs_pidns_fops) return NULL;
	return (pid_namespace_t *)f->priv;
}

user_namespace_t *nsfile_to_user(vfs_file_t *f)
{
	if (!f || !f->inode || f->inode->fops != &procfs_userns_fops) return NULL;
	return (user_namespace_t *)f->priv;
}

net_namespace_t *nsfile_to_net(vfs_file_t *f)
{
	if (!f || !f->inode || f->inode->fops != &procfs_netns_fops) return NULL;
	return (net_namespace_t *)f->priv;
}

struct cgroup_namespace *nsfile_to_cgroup(vfs_file_t *f)
{
	if (!f || !f->inode || f->inode->fops != &procfs_cgroupns_fops) return NULL;
	return (struct cgroup_namespace *)f->priv;
}

static vfs_inode_t *procfs_nslink_inode(int pid, int kind)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	/* Like Linux nsfs: the inode number is the namespace's identity, so
	 * stat() on /proc/A/ns/mnt and /proc/B/ns/mnt agree when A and B share
	 * it (gdb's linux_ns_same relies on this). */
	unsigned long nsid = nslink_inum(pid, kind, NULL);
	in->ino      = nsid ? nsid : proc_ino_alloc();
	in->mode     = S_IFLNK | 0777;
	in->nlink    = 1;
	in->refcount = 1;
	in->iops     = &procfs_nslink_iops;
	in->fops     = (kind == NS_IPC)  ? &procfs_ipcns_fops  :
	               (kind == NS_TIME) ? &procfs_timens_fops :
	               (kind == NS_MNT)  ? &procfs_mntns_fops  :
	               (kind == NS_PID)  ? &procfs_pidns_fops  :
	               (kind == NS_USER) ? &procfs_userns_fops :
	               (kind == NS_NET)  ? &procfs_netns_fops  :
	               (kind == NS_CGROUP) ? &procfs_cgroupns_fops : &procfs_utsns_fops;
	procfs_nslink_priv_t *pp = kmalloc(sizeof(*pp));
	if (!pp) { kfree(in); return NULL; }
	pp->pid = pid; pp->kind = kind;
	in->priv = pp;
	return in;
}

static vfs_inode_t *procfs_nsdir_lookup(vfs_inode_t *dir, const char *name)
{
	procfs_fddir_priv_t *pp = dir->priv;
	int pid = pp ? pp->pid : 0;
	for (int k = 0; k < NS_KINDS_N; k++)
		if (strcmp(name, ns_kinds[k]) == 0) return procfs_nslink_inode(pid, k);
	return NULL;
}

static int procfs_nsdir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	if (idx >= (uint32_t)NS_KINDS_N) return -1;
	procfs_fddir_priv_t *pp = dir->priv;
	int pid = pp ? pp->pid : 0;
	const char *n = ns_kinds[idx];
	int nlen = 0; while (n[nlen]) nlen++;
	out->ino      = 0x8000 + (uint64_t)pid * 100 + 80 + idx;
	out->mode     = S_IFLNK | 0777;
	out->next_off = idx + 1;
	memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	return 0;
}

static vfs_iops_t procfs_nsdir_iops = {
	.lookup  = procfs_nsdir_lookup,
	.readdir = procfs_nsdir_readdir,
};

static vfs_inode_t *procfs_nsdir_inode(int pid)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = proc_ino_alloc();
	in->mode     = S_IFDIR | 0555;
	in->nlink    = 2;
	in->refcount = 1;
	in->iops     = &procfs_nsdir_iops;
	in->fops     = &procfs_dir_fops;
	procfs_fddir_priv_t *pp = kmalloc(sizeof(*pp));
	if (!pp) { kfree(in); return NULL; }
	pp->pid = pid;
	in->priv = pp;
	return in;
}

/* ── /proc/<pid>/timens_offsets — read/write the time ns offsets ── */

static int procfs_off_fmt(char *b, const char *name, long sec, long nsec)
{
	int i = 0;
	for (const char *s = name; *s; s++) b[i++] = *s;
	b[i++] = ' ';
	long v = sec;
	if (v < 0) { b[i++] = '-'; v = -v; }
	char rev[24]; int r = 0;
	if (v == 0) rev[r++] = '0';
	while (v) { rev[r++] = '0' + (int)(v % 10); v /= 10; }
	while (r) b[i++] = rev[--r];
	b[i++] = ' ';
	v = nsec; r = 0;
	if (v < 0) { b[i++] = '-'; v = -v; }
	if (v == 0) rev[r++] = '0';
	while (v) { rev[r++] = '0' + (int)(v % 10); v /= 10; }
	while (r) b[i++] = rev[--r];
	b[i++] = '\n';
	return i;
}

static ssize_t procfs_timens_off_read(vfs_file_t *f, void *buf, size_t len)
{
	procfs_fddir_priv_t *pr = f->inode->priv;
	int pid = pr ? pr->pid : 0;
	process_t *p = pid == 0 ? task_current() : proc_get((pid_t)pid);
	if (!p) return -1;
	time_namespace_t *ns = p->time_ns_new ? p->time_ns_new
	                     : (p->time_ns ? p->time_ns : &init_time_ns);
	char tmp[96]; int n = 0;
	n += procfs_off_fmt(tmp + n, "monotonic", ns->mono_sec, ns->mono_nsec);
	n += procfs_off_fmt(tmp + n, "boottime",  ns->boot_sec, ns->boot_nsec);
	if (pid) proc_put(p);
	if (f->pos >= (off_t)n) return 0;
	size_t avail = (size_t)(n - f->pos);
	if (len > avail) len = avail;
	if (copy_fop_to_user(buf, tmp + f->pos, len) != 0) return -EFAULT;
	f->pos += (off_t)len;
	return (ssize_t)len;
}

static ssize_t procfs_timens_off_write(vfs_file_t *f, const void *buf, size_t len)
{
	procfs_fddir_priv_t *pr = f->inode->priv;
	int pid = pr ? pr->pid : 0;
	process_t *p = pid == 0 ? task_current() : proc_get((pid_t)pid);
	if (!p) return -1;
	/* offsets may only be set on a pending time ns (one no task has joined) */
	if (!p->time_ns_new) { if (pid) proc_put(p); return -EACCES; } /* EACCES */

	char kb[128];
	if (len >= sizeof(kb)) len = sizeof(kb) - 1;
	if (copy_fop_from_user(kb, buf, len) != 0) { if (pid) proc_put(p); return -EFAULT; }
	kb[len] = '\0';

	const char *s = kb;
	while (*s) {
		while (*s == ' ' || *s == '\t' || *s == '\n') s++;
		if (!*s) break;
		char first = *s;
		while (*s && *s != ' ' && *s != '\t') s++;          /* skip name */
		while (*s == ' ' || *s == '\t') s++;
		int neg = 0; if (*s == '-') { neg = 1; s++; }
		long sec = 0; while (*s >= '0' && *s <= '9') sec = sec * 10 + (*s++ - '0');
		if (neg) sec = -sec;
		while (*s == ' ' || *s == '\t') s++;
		int neg2 = 0; if (*s == '-') { neg2 = 1; s++; }
		long nsec = 0; while (*s >= '0' && *s <= '9') nsec = nsec * 10 + (*s++ - '0');
		if (neg2) nsec = -nsec;
		while (*s && *s != '\n') s++;

		vdso_timens_used = 1;
		if (first == 'm') { p->time_ns_new->mono_sec = sec; p->time_ns_new->mono_nsec = nsec; }
		else if (first == 'b') { p->time_ns_new->boot_sec = sec; p->time_ns_new->boot_nsec = nsec; }
	}
	if (pid) proc_put(p);
	return (ssize_t)len;
}

static vfs_fops_t procfs_timens_off_fops = {
	.read  = procfs_timens_off_read,
	.write = procfs_timens_off_write,
};

static vfs_inode_t *procfs_timens_off_inode(int pid)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = proc_ino_alloc();
	in->mode     = S_IFREG | 0644;
	in->nlink    = 1;
	in->refcount = 1;
	in->fops     = &procfs_timens_off_fops;
	procfs_fddir_priv_t *pp = kmalloc(sizeof(*pp));
	if (!pp) { kfree(in); return NULL; }
	pp->pid = pid;
	in->priv = pp;
	return in;
}

/* ── /proc/<pid>/{uid,gid}_map — user namespace id mappings ──── */

typedef struct { int pid; int is_gid; } procfs_uidmap_priv_t;

static ssize_t procfs_uidmap_read(vfs_file_t *f, void *buf, size_t len)
{
	procfs_uidmap_priv_t *pr = f->inode->priv;
	process_t *p = pr->pid == 0 ? task_current() : proc_get((pid_t)pr->pid);
	if (!p) return -1;
	user_namespace_t *ns = p->user_ns ? p->user_ns : &init_user_ns;
	char tmp[256];
	int n = user_ns_format_map(ns, tmp, sizeof(tmp), pr->is_gid);
	if (pr->pid) proc_put(p);
	if (f->pos >= (off_t)n) return 0;
	size_t avail = (size_t)(n - f->pos);
	if (len > avail) len = avail;
	if (copy_to_user(buf, tmp + f->pos, len) != 0) return -EFAULT;
	f->pos += (off_t)len;
	return (ssize_t)len;
}

static ssize_t procfs_uidmap_write(vfs_file_t *f, const void *buf, size_t len)
{
	procfs_uidmap_priv_t *pr = f->inode->priv;
	process_t *p = pr->pid == 0 ? task_current() : proc_get((pid_t)pr->pid);
	if (!p) return -1;
	if (!p->user_ns) { if (pr->pid) proc_put(p); return -1; }   /* only a non-initial user ns is mappable */
	char kb[256];
	size_t n = len < sizeof(kb) - 1 ? len : sizeof(kb) - 1;
	if (copy_from_user(kb, buf, n) != 0) { if (pr->pid) proc_put(p); return -EFAULT; }
	kb[n] = '\0';
	int rc = user_ns_map_permitted(task_current(), p->user_ns, kb, pr->is_gid)
	         ? user_ns_set_map(p->user_ns, kb, pr->is_gid) : -1;
	if (pr->pid) proc_put(p);
	return rc != 0 ? -1 : (ssize_t)len;
}

static vfs_fops_t procfs_uidmap_fops = { .read = procfs_uidmap_read, .write = procfs_uidmap_write };

static vfs_inode_t *procfs_uidmap_inode(int pid, int is_gid)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = proc_ino_alloc();
	in->mode     = S_IFREG | 0644;
	in->nlink    = 1;
	in->refcount = 1;
	in->fops     = &procfs_uidmap_fops;
	/* the task's own, as on Linux: its user may open it to map itself */
	process_t *t = pid ? proc_get((pid_t)pid) : task_current();
	if (t) { in->uid = t->euid; in->gid = t->egid; if (pid) proc_put(t); }
	procfs_uidmap_priv_t *pr = kmalloc(sizeof(*pr));
	if (!pr) { kfree(in); return NULL; }
	pr->pid = pid; pr->is_gid = is_gid;
	in->priv = pr;
	return in;
}

/* ── /proc/<pid>/cgroup — cgroup v2 membership ("0::<path>") ── */

static ssize_t procfs_cgroup_read(vfs_file_t *f, void *buf, size_t len)
{
	procfs_fddir_priv_t *pr = f->inode->priv;
	int pid = pr ? pr->pid : 0;
	process_t *p = pid == 0 ? task_current() : proc_get((pid_t)pid);
	if (!p) return -1;
	char tmp[160];
	tmp[0] = '0'; tmp[1] = ':'; tmp[2] = ':';
	int n = 3 + cgroup_proc_path(p, tmp + 3, (int)sizeof(tmp) - 4);
	tmp[n++] = '\n';
	if (pid) proc_put(p);
	if (f->pos >= (off_t)n) return 0;
	size_t avail = (size_t)(n - f->pos);
	if (len > avail) len = avail;
	if (copy_to_user(buf, tmp + f->pos, len) != 0) return -EFAULT;
	f->pos += (off_t)len;
	return (ssize_t)len;
}

static vfs_fops_t procfs_cgroup_fops = { .read = procfs_cgroup_read };

static vfs_inode_t *procfs_cgroup_inode(int pid)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = proc_ino_alloc();
	in->mode     = S_IFREG | 0444;
	in->nlink    = 1;
	in->refcount = 1;
	in->fops     = &procfs_cgroup_fops;
	procfs_fddir_priv_t *pp = kmalloc(sizeof(*pp));
	if (!pp) { kfree(in); return NULL; }
	pp->pid = pid;
	in->priv = pp;
	return in;
}

/* ── /proc/asess/<id>/, /proc/<pid>/asess/ ───────────────────── */

static int fill_asess_status(uint64_t id, char *buf, size_t sz)
{
	static const char *const states[] = { "created", "running", "dead" };
	asess_view_t v;
	if (asess_view(id, &v) != 0) return 0;
	pb_t b = { buf, sz };
	pb_str(&b, "id: ");        pb_u64(&b, v.id);        pb_char(&b, '\n');
	pb_str(&b, "label: ");     pb_str(&b, v.label);     pb_char(&b, '\n');
	pb_str(&b, "state: ");     pb_str(&b, states[v.state]); pb_char(&b, '\n');
	pb_str(&b, "uid: ");       pb_u64(&b, v.uid);       pb_char(&b, '\n');
	pb_str(&b, "tasks: ");     pb_u64(&b, v.tasks);     pb_char(&b, '\n');
	pb_str(&b, "max_tasks: "); pb_u64(&b, v.max_tasks); pb_char(&b, '\n');
	pb_str(&b, "denied: ");    pb_u64(&b, v.denied);    pb_char(&b, '\n');
	pb_str(&b, "mem_cur: ");   pb_u64(&b, (uint64_t)v.mem_cur); pb_char(&b, '\n');
	pb_str(&b, "mem_max: ");   pb_u64(&b, (uint64_t)v.mem_max); pb_char(&b, '\n');
	return (int)(sz - b.rem);
}

/* one line per task, numbered as the reader's pid namespace numbers it */
static int fill_asess_tasks(uint64_t id, char *buf, size_t sz)
{
	asess_view_t v;
	if (asess_view(id, &v) != 0) return 0;
	pb_t b = { buf, sz };
	pid_namespace_t *ns = current_pid_ns();
	int n = task_count();
	for (int i = 0; i < n; i++) {
		process_t *p = task_get(i);
		if (!p || asess_id_of(p) != id) continue;
		int nr = task_pid_nr_ns(p, ns);
		if (nr) { pb_u64(&b, (uint64_t)nr); pb_char(&b, '\n'); }
	}
	return (int)(sz - b.rem);
}

static int fill_asess_denied(uint64_t id, char *buf, size_t sz)
{
	asess_view_t v;
	if (asess_view(id, &v) != 0) return 0;
	pb_t b = { buf, sz };
	for (uint32_t i = 0; i < v.nrecent; i++) {
		pb_str(&b, "rule=gate syscall="); pb_u64(&b, (uint64_t)v.recent[i].nr);
		pb_str(&b, " pid=");              pb_u64(&b, (uint64_t)v.recent[i].pid);
		pb_char(&b, '\n');
	}
	return (int)(sz - b.rem);
}

static const struct { const char *name; int type; } asess_files[] = {
	{ "status", PROC_F_ASESS_STATUS },
	{ "tasks",  PROC_F_ASESS_TASKS  },
	{ "denied", PROC_F_ASESS_DENIED },
};
#define ASESS_FILES_N ((int)(sizeof(asess_files) / sizeof(asess_files[0])))

static vfs_inode_t *procfs_asessdir_lookup(vfs_inode_t *dir, const char *name)
{
	procfs_fddir_priv_t *pp = dir->priv;
	for (int i = 0; pp && i < ASESS_FILES_N; i++)
		if (strcmp(name, asess_files[i].name) == 0)
			return procfs_inode_file(pp->pid, asess_files[i].type);
	return NULL;
}

static int procfs_asessdir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= (uint32_t)ASESS_FILES_N) return -1;
	const char *n = asess_files[idx].name;
	int nlen = 0; while (n[nlen]) nlen++;
	out->ino      = proc_ino_alloc();
	out->mode     = S_IFREG | 0444;
	out->next_off = idx + 1;
	memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	return 0;
}

static vfs_iops_t procfs_asessdir_iops = {
	.lookup  = procfs_asessdir_lookup,
	.readdir = procfs_asessdir_readdir,
};

static vfs_inode_t *procfs_asessdir_inode(uint64_t id)
{
	asess_view_t v;
	if (!id || asess_view(id, &v) != 0) return NULL;
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino      = proc_ino_alloc();
	in->mode     = S_IFDIR | 0555;
	in->nlink    = 2;
	in->refcount = 1;
	in->iops     = &procfs_asessdir_iops;
	in->fops     = &procfs_dir_fops;
	in->no_cache = 1;
	procfs_fddir_priv_t *pp = kmalloc(sizeof(*pp));
	if (!pp) { kfree(in); return NULL; }
	pp->pid  = (int)id;
	in->priv = pp;
	return in;
}

static vfs_inode_t *procfs_asessroot_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	uint64_t id = 0;
	const char *p = name;
	while (*p >= '0' && *p <= '9') id = id * 10 + (uint64_t)(*p++ - '0');
	return *p || p == name ? NULL : procfs_asessdir_inode(id);
}

static int procfs_asessroot_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	uint64_t id = asess_nth_visible(idx);
	if (!id) return -1;
	char rev[20]; int r = 0, n = 0;
	do { rev[r++] = (char)('0' + id % 10); id /= 10; } while (id);
	while (r) out->name[n++] = rev[--r];
	out->name[n] = '\0';
	out->ino      = proc_ino_alloc();
	out->mode     = S_IFDIR | 0555;
	out->next_off = idx + 1;
	return 0;
}

static vfs_iops_t procfs_asessroot_iops = {
	.lookup  = procfs_asessroot_lookup,
	.readdir = procfs_asessroot_readdir,
};

static vfs_inode_t *procfs_asessroot_inode(void)
{
	static vfs_inode_t d;
	if (!d.ino) {
		d.ino = 0xb000; d.mode = S_IFDIR | 0555; d.nlink = 2; d.refcount = 1;
		d.iops = &procfs_asessroot_iops; d.fops = &procfs_dir_fops;
		d.no_cache = 1; d.cached = 1;
	}
	return &d;
}

static vfs_inode_t *procfs_taskdir_inode(int pid);

static vfs_inode_t *procfs_piddir_lookup(vfs_inode_t *dir, const char *name)
{
	procfs_piddir_priv_t *pp = dir->priv;
	if (!pp) return NULL;
	int pid = pp->pid;
	if (strcmp(name, "task") == 0)
		return procfs_taskdir_inode(pid);
	if (strcmp(name, "fd") == 0)
		return procfs_fddir_inode(pid);
	if (strcmp(name, "fdinfo") == 0)
		return procfs_fdinfodir_inode(pid);
	if (strcmp(name, "ns") == 0)
		return procfs_nsdir_inode(pid);
	if (strcmp(name, "timens_offsets") == 0)
		return procfs_timens_off_inode(pid);
	if (strcmp(name, "uid_map") == 0)
		return procfs_uidmap_inode(pid, 0);
	if (strcmp(name, "gid_map") == 0)
		return procfs_uidmap_inode(pid, 1);
	if (strcmp(name, "cgroup") == 0)
		return procfs_cgroup_inode(pid);
	if (strcmp(name, "mem") == 0)
		return procfs_mem_inode(pid);
	if (strcmp(name, "asess") == 0) {
		process_t *t = pid ? proc_get((pid_t)pid) : task_current();
		uint64_t id = t ? asess_id_of(t) : 0;
		if (pid) proc_put(t);
		return procfs_asessdir_inode(id);
	}
	for (int i = 0; i < PID_LINKS_N; i++) {
		if (strcmp(pid_links[i], name) == 0)
			return procfs_pidlink_inode(pid, i);
	}
	for (int i = 0; i < PID_FILES_N; i++) {
		if (strcmp(pid_files[i].name, name) == 0)
			return procfs_inode_file(pid, pid_files[i].type);
	}
	return NULL;
}

static int procfs_piddir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	if (idx >= (uint32_t)(PID_FILES_N + 4 + PID_LINKS_N)) return -1;
	procfs_piddir_priv_t *pp = dir->priv;
	int pid = pp ? pp->pid : 0;
	if (idx < PID_FILES_N) {
		out->ino      = 0x8000 + (uint64_t)pid * 100 + idx;
		out->mode     = S_IFREG | 0444;
		out->next_off = idx + 1;
		const char *n = pid_files[idx].name;
		int nlen = 0; while (n[nlen]) nlen++;
		memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	} else if (idx == PID_FILES_N) {
		out->ino      = 0x8000 + (uint64_t)pid * 100 + 99;
		out->mode     = S_IFDIR | 0555;
		out->next_off = idx + 1;
		memcpy(out->name, "fd", 3);
	} else if (idx < (uint32_t)(PID_FILES_N + 1 + PID_LINKS_N)) {
		const char *n = pid_links[idx - PID_FILES_N - 1];
		int nlen = 0; while (n[nlen]) nlen++;
		out->ino      = 0x8000 + (uint64_t)pid * 100 + 90 + (idx - PID_FILES_N - 1);
		out->mode     = S_IFLNK | 0777;
		out->next_off = idx + 1;
		memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	} else if (idx == (uint32_t)(PID_FILES_N + 1 + PID_LINKS_N)) {
		out->ino      = 0x8000 + (uint64_t)pid * 100 + 81;
		out->mode     = S_IFDIR | 0555;
		out->next_off = idx + 1;
		memcpy(out->name, "ns", 3);
	} else if (idx == (uint32_t)(PID_FILES_N + 2 + PID_LINKS_N)) {
		out->ino      = 0x8000 + (uint64_t)pid * 100 + 82;
		out->mode     = S_IFREG | 0644;
		out->next_off = idx + 1;
		memcpy(out->name, "timens_offsets", 15);
	} else {
		out->ino      = 0x8000 + (uint64_t)pid * 100 + 70;
		out->mode     = S_IFDIR | 0555;
		out->next_off = idx + 1;
		memcpy(out->name, "task", 5);
	}
	return 0;
}

static int procfs_piddir_stat(vfs_inode_t *in, vfs_stat_t *out)
{
	out->ino   = in->ino;
	out->mode  = in->mode;
	out->nlink = in->nlink;
	out->size  = 0;
	return 0;
}

static vfs_iops_t procfs_piddir_iops = {
	.lookup  = procfs_piddir_lookup,
	.readdir = procfs_piddir_readdir,
	.stat    = procfs_piddir_stat,
};

static vfs_inode_t *procfs_piddir_inode(int pid)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino     = 0x8000 + (uint64_t)pid * 100 + 50;
	in->mode    = S_IFDIR | 0555;
	in->nlink   = 2;
	in->refcount= 1;
	in->iops    = &procfs_piddir_iops;
	in->fops    = &procfs_dir_fops;

	procfs_piddir_priv_t *pp = kmalloc(sizeof(*pp));
	if (!pp) { kfree(in); return NULL; }
	pp->pid  = pid;
	in->priv = pp;
	return in;
}

/* ── /proc/<pid>/task/ — per-thread directories ──────────────────
 * task/<tid>/ is itself a pid-dir (same files as /proc/<tid>). ps -T,
 * top -H and gdb thread enumeration read this; we list every live thread
 * of the tgid, with the main thread (tid == tgid) always present. */
static vfs_inode_t *procfs_taskdir_lookup(vfs_inode_t *dir, const char *name)
{
	procfs_piddir_priv_t *pp = dir->priv;
	if (!pp) return NULL;
	int tid = 0;
	for (const char *c = name; *c; c++) {
		if (*c < '0' || *c > '9') return NULL;
		tid = tid * 10 + (*c - '0');
	}
	process_t *t = proc_get((pid_t)tid);
	if (!t) return NULL;
	/* a thread that has exited is gone from task/, though its slot may wait
	 * to be reaped: Chromium stops a thread and polls for task/<tid> to vanish */
	int ok = (t->tgid == (pid_t)pp->pid) && t->state != PROC_DEAD &&
	         (t->state != PROC_ZOMBIE || t->pid == t->tgid);
	proc_put(t);
	if (!ok) return NULL;
	vfs_inode_t *in = procfs_piddir_inode(tid);
	/* distinct ino from /proc/<tid>: the leader's task/<tid> shares pid==tid and
	 * would otherwise alias the top-level pid dir. */
	if (in) in->ino = proc_ino_alloc();
	return in;
}
/* Enumerate the tgid's live threads. Each readdir(idx) returns the idx-th
 * matching thread; the leader (tid == tgid) is index 0 and always present. */
static int procfs_taskdir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	procfs_piddir_priv_t *pp = dir->priv;
	int tgid = pp ? pp->pid : 0;
	int cnt = task_count();
	uint32_t seen = 0;
	for (int i = 0; i < cnt; i++) {
		process_t *p = task_get(i);
		if (!p) continue;
		if (p->tgid != (pid_t)tgid) continue;
		if (p->state == PROC_DEAD || p->state == PROC_ZOMBIE) continue;
		if (seen == idx) {
			uint64_t tid = (uint64_t)p->pid;
			out->ino = 0x8000 + tid * 100 + 70;
			out->mode = S_IFDIR | 0555; out->next_off = idx + 1;
			int nl = 0; char rev[20]; uint64_t v = tid;
			if (v == 0) rev[nl++] = '0';
			while (v) { rev[nl++] = (char)('0' + v % 10); v /= 10; }
			int o = 0; for (int j = nl - 1; j >= 0; j--) out->name[o++] = rev[j];
			out->name[o] = '\0';
			return 0;
		}
		seen++;
	}
	return -1;
}
/* nlink is 2 + live threads, as on Linux: Chromium's sandbox tells "one
 * thread left" by st_nlink == 3 and waits for a stopped thread to leave. */
static int procfs_taskdir_stat(vfs_inode_t *in, vfs_stat_t *out)
{
	procfs_piddir_priv_t *pp = in->priv;
	int tgid = pp ? pp->pid : 0;
	int cnt = task_count(), n = 0;
	for (int i = 0; i < cnt; i++) {
		process_t *p = task_get(i);
		if (!p || p->tgid != (pid_t)tgid) continue;
		if (p->state == PROC_DEAD || p->state == PROC_ZOMBIE) continue;
		n++;
	}
	out->ino = in->ino; out->mode = in->mode; out->nlink = 2 + (n ? n : 1); out->size = 0;
	return 0;
}
static vfs_iops_t procfs_taskdir_iops = {
	.lookup  = procfs_taskdir_lookup,
	.readdir = procfs_taskdir_readdir,
	.stat    = procfs_taskdir_stat,
};
static vfs_inode_t *procfs_taskdir_inode(int pid)
{
	vfs_inode_t *in = kmalloc(sizeof(*in));
	if (!in) return NULL;
	memset(in, 0, sizeof(*in));
	in->ino     = proc_ino_alloc();
	in->mode    = S_IFDIR | 0555;
	in->nlink   = 2;
	in->refcount= 1;
	in->iops    = &procfs_taskdir_iops;
	in->fops    = &procfs_dir_fops;
	procfs_piddir_priv_t *pp = kmalloc(sizeof(*pp));
	if (!pp) { kfree(in); return NULL; }
	pp->pid  = pid;
	in->priv = pp;
	return in;
}

/* ── /proc/net fill functions ────────────────────────────────── */

static void pb_hex8(pb_t *b, uint32_t v)
{
	static const char hx[] = "0123456789ABCDEF";
	for (int i = 7; i >= 0; i--)
		pb_char(b, hx[(v >> (i * 4)) & 0xF]);
}

static void pb_hex4(pb_t *b, uint16_t v)
{
	static const char hx[] = "0123456789ABCDEF";
	pb_char(b, hx[(v >> 12) & 0xF]);
	pb_char(b, hx[(v >>  8) & 0xF]);
	pb_char(b, hx[(v >>  4) & 0xF]);
	pb_char(b, hx[(v      ) & 0xF]);
}

/* Right-justified columns; a value wider than the column still prints whole. */
static void pb_col(pb_t *b, const char *s, int width)
{
	int n = (int)strlen(s);
	while (n++ < width) pb_char(b, ' ');
	pb_str(b, s);
}

static void pb_u64col(pb_t *b, uint64_t v, int width)
{
	char tmp[21];
	pb_t t = {tmp, sizeof(tmp) - 1};
	pb_u64(&t, v);
	tmp[sizeof(tmp) - 1 - t.rem] = '\0';
	pb_col(b, tmp, width);
}

static int fill_net_dev(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "Inter-|   Receive                                                |  Transmit\n");
	pb_str(&b, " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
	for (int i = 0; ; i++) {
		netdev_t *dev = net_dev(i);
		if (!dev) break;
		pb_col(&b, dev->name, 6);
		pb_char(&b, ':');
		pb_u64col(&b, dev->rx_bytes, 8);
		pb_u64col(&b, dev->rx_packets, 8);
		pb_u64col(&b, 0, 5);                 /* errs */
		pb_u64col(&b, dev->rx_dropped, 5);
		pb_str(&b, "    0     0          0         0");   /* fifo..multicast */
		pb_u64col(&b, dev->tx_bytes, 9);
		pb_u64col(&b, dev->tx_packets, 8);
		pb_u64col(&b, 0, 5);                 /* errs */
		pb_u64col(&b, dev->tx_dropped, 5);
		pb_str(&b, "    0     0       0          0\n");   /* fifo..compressed */
	}
	return (int)(sz - b.rem);
}

/* Linux's numbers for the "st" column (include/net/tcp_states.h); ss, netstat and
 * door listings read LISTEN as 0A. Indexed by our TCP_* (kernel/net/socket.h). */
static const uint8_t linux_tcp_state[] = {
	[TCP_CLOSED] = 0x07, [TCP_LISTEN] = 0x0A, [TCP_SYN_SENT] = 0x02, [TCP_SYN_RECV] = 0x03,
	[TCP_ESTABLISHED] = 0x01, [TCP_FIN_WAIT1] = 0x04, [TCP_FIN_WAIT2] = 0x05,
	[TCP_CLOSE_WAIT] = 0x08, [TCP_CLOSING] = 0x0B, [TCP_LAST_ACK] = 0x09, [TCP_TIME_WAIT] = 0x06,
};

static int fill_net_tcp(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
	int sl = 0;
	for (int i = 0; i < gsock_cap; i++) {
		socket_t *s = gsock[i];
		if (!s || s->type != SOCK_STREAM || s->domain != AF_INET) continue;
		/* format: "   N: LLLLLLLL:PPPP RRRRRRRR:PPPP SS 00000000:00000000 ..." */
		pb_str(&b, "   ");
		pb_u64(&b, (uint64_t)sl);
		pb_str(&b, ": ");
		/* local addr in network byte order (little-endian hex = host byte order reversed) */
		uint32_t la = s->local_ip;
		pb_hex8(&b, ((la & 0xFF) << 24) | (((la >> 8) & 0xFF) << 16) |
		             (((la >> 16) & 0xFF) << 8) | ((la >> 24) & 0xFF));
		pb_char(&b, ':');
		pb_hex4(&b, s->local_port);
		pb_str(&b, " ");
		uint32_t ra = s->remote_ip;
		pb_hex8(&b, ((ra & 0xFF) << 24) | (((ra >> 8) & 0xFF) << 16) |
		             (((ra >> 16) & 0xFF) << 8) | ((ra >> 24) & 0xFF));
		pb_char(&b, ':');
		pb_hex4(&b, s->remote_port);
		pb_str(&b, " ");
		unsigned st = (unsigned)s->tcp_state < sizeof linux_tcp_state ? linux_tcp_state[s->tcp_state] : 0x07;
		static const char hx[] = "0123456789ABCDEF";
		pb_char(&b, hx[st >> 4]);
		pb_char(&b, hx[st & 0xF]);
		pb_str(&b, " 00000000:00000000 00:00000000 00000000 ");
		pb_u64col(&b, s->own_uid, 5);       /* who opened it: a door has an owner */
		pb_str(&b, "        0 ");
		pb_u64(&b, (uint64_t)(0x10000 + i));
		pb_str(&b, " 1 0000000000000000 100 0 0 10 0\n");
		sl++;
	}
	return (int)(sz - b.rem);
}

static int fill_net_udp(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
	int sl = 0;
	for (int i = 0; i < gsock_cap; i++) {
		socket_t *s = gsock[i];
		if (!s || s->type != SOCK_DGRAM || s->domain != AF_INET) continue;
		pb_str(&b, "   ");
		pb_u64(&b, (uint64_t)sl);
		pb_str(&b, ": ");
		uint32_t la = s->local_ip;
		pb_hex8(&b, ((la & 0xFF) << 24) | (((la >> 8) & 0xFF) << 16) |
		             (((la >> 16) & 0xFF) << 8) | ((la >> 24) & 0xFF));
		pb_char(&b, ':');
		pb_hex4(&b, s->local_port);
		pb_str(&b, " 00000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 ");
		pb_u64(&b, (uint64_t)(0x10000 + i));
		pb_str(&b, " 1 0000000000000000\n");
		sl++;
	}
	return (int)(sz - b.rem);
}

static int fill_net_route(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
	netdev_t *dev = net_dev(0);
	if (dev) {
		/* default route */
		pb_str(&b, "eth0\t00000000\t");
		uint32_t gw = dev->gw;
		pb_hex8(&b, ((gw & 0xFF) << 24) | (((gw >> 8) & 0xFF) << 16) |
		             (((gw >> 16) & 0xFF) << 8) | ((gw >> 24) & 0xFF));
		pb_str(&b, "\t0003\t0\t0\t0\t00000000\t0\t0\t0\n");
		/* local subnet */
		pb_str(&b, "eth0\t");
		uint32_t net = dev->ip & dev->mask;
		pb_hex8(&b, ((net & 0xFF) << 24) | (((net >> 8) & 0xFF) << 16) |
		             (((net >> 16) & 0xFF) << 8) | ((net >> 24) & 0xFF));
		pb_str(&b, "\t00000000\t0001\t0\t0\t0\t");
		pb_hex8(&b, ((dev->mask & 0xFF) << 24) | (((dev->mask >> 8) & 0xFF) << 16) |
		             (((dev->mask >> 16) & 0xFF) << 8) | ((dev->mask >> 24) & 0xFF));
		pb_str(&b, "\t0\t0\t0\n");
	}
	return (int)(sz - b.rem);
}

/* one /proc/net/if_inet6 line: 32-hex addr, ifindex, prefixlen, scope, flags, name */
static void pb_if6(pb_t *b, const in6_addr_t *a, int ifindex,
                   int prefixlen, int scope, const char *name)
{
	static const char hx[] = "0123456789abcdef";
	for (int i = 0; i < 16; i++) {
		pb_char(b, hx[(a->s6[i] >> 4) & 0xf]);
		pb_char(b, hx[a->s6[i] & 0xf]);
	}
	pb_char(b, ' '); pb_char(b, hx[(ifindex >> 4) & 0xf]); pb_char(b, hx[ifindex & 0xf]);
	pb_char(b, ' '); pb_char(b, hx[(prefixlen >> 4) & 0xf]); pb_char(b, hx[prefixlen & 0xf]);
	pb_char(b, ' '); pb_char(b, hx[(scope >> 4) & 0xf]); pb_char(b, hx[scope & 0xf]);
	pb_char(b, ' '); pb_char(b, '8'); pb_char(b, '0');   /* IFA_F_PERMANENT */
	pb_char(b, ' '); pb_char(b, ' '); pb_char(b, ' '); pb_char(b, ' ');
	pb_str(b, name); pb_char(b, '\n');
}

static int fill_net_if_inet6(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_if6(&b, &in6addr_loopback, 1, 128, 0x10, "lo");   /* host scope */
	for (int i = 0; ; i++) {
		netdev_t *dev = net_dev(i);
		if (!dev) break;
		if ((dev->ip >> 24) == 127) continue;   /* loopback: only ::1, above */
		int ifindex = i + 2;
		if (in6_is_ll(&dev->ip6_ll))
			pb_if6(&b, &dev->ip6_ll, ifindex, 64, 0x20, dev->name);  /* link scope */
		if (!in6_is_unspec(&dev->ip6))
			pb_if6(&b, &dev->ip6, ifindex, 64, 0x00, dev->name);     /* global scope */
	}
	return (int)(sz - b.rem);
}

/* /proc/net/unix: Num RefCount Protocol Flags Type St Inode Path
 * (ss -x / netstat -x / lsof resolve AF_UNIX sockets from here). */
static int fill_net_unix(char *buf, size_t sz)
{
	pb_t b = {buf, sz};
	pb_str(&b, "Num       RefCount Protocol Flags    Type St Inode Path\n");
	for (int i = 0; i < gsock_cap; i++) {
		socket_t *s = gsock[i];
		if (!s || s->domain != AF_UNIX) continue;
		int listening = (s->type == SOCK_STREAM && s->tcp_state == TCP_LISTEN);
		int connected = (s->unix_peer != NULL);
		/* kernel object address (Num) */
		pb_str(&b, "0000000000000000: ");
		pb_str(&b, "0000000"); pb_u64(&b, (uint64_t)s->refcount); pb_char(&b, ' ');
		pb_str(&b, "00000000 ");                                  /* Protocol */
		pb_str(&b, listening ? "00010000 " : "00000000 ");        /* Flags (ACC for listeners) */
		pb_hex8(&b, (uint32_t)(s->type & SOCK_TYPE_MASK));        /* Type */
		pb_char(&b, ' ');
		pb_str(&b, listening ? "01" : connected ? "03" : "01");   /* St: LISTEN/CONNECTED/UNCONNECTED */
		pb_char(&b, ' ');
		pb_u64(&b, (uint64_t)(0x10000 + i));                      /* Inode (synthesized) */
		if (s->unix_path_len > 0 && s->unix_path[0]) {
			pb_char(&b, ' ');
			pb_str(&b, s->unix_path);
		} else if (s->unix_path_len > 0) {
			pb_str(&b, " @");                                     /* abstract namespace */
		}
		pb_char(&b, '\n');
	}
	return (int)(sz - b.rem);
}

static const struct { const char *name; int type; } net_files[] = {
	{ "dev",     PROC_F_NET_DEV   },
	{ "tcp",     PROC_F_NET_TCP   },
	{ "udp",     PROC_F_NET_UDP   },
	{ "route",   PROC_F_NET_ROUTE },
	{ "if_inet6",PROC_F_NET_IF6   },
	{ "unix",    PROC_F_NET_UNIX  },
};
#define NET_FILES_N 6

static vfs_inode_t *procfs_netdir_inode(void);

static vfs_inode_t *procfs_netdir_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	for (int i = 0; i < NET_FILES_N; i++)
		if (strcmp(net_files[i].name, name) == 0)
			return procfs_inode_file(-1, net_files[i].type);
	return NULL;
}

static int procfs_netdir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= (uint32_t)NET_FILES_N) return -1;
	out->ino      = 0x9000 + idx;
	out->mode     = S_IFREG | 0444;
	out->next_off = idx + 1;
	const char *n = net_files[idx].name;
	int nlen = 0; while (n[nlen]) nlen++;
	memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	return 0;
}

static int procfs_netdir_stat(vfs_inode_t *in, vfs_stat_t *out)
{
	out->ino = in->ino; out->mode = in->mode; out->nlink = 2; out->size = 0;
	return 0;
}

static vfs_iops_t procfs_netdir_iops = {
	.lookup  = procfs_netdir_lookup,
	.readdir = procfs_netdir_readdir,
	.stat    = procfs_netdir_stat,
};

static vfs_inode_t *procfs_netdir_inode(void)
{
	static vfs_inode_t ndir;
	static int init = 0;
	if (!init) {
		memset(&ndir, 0, sizeof(ndir));
		ndir.ino      = 0x9000;
		ndir.mode     = S_IFDIR | 0555;
		ndir.nlink    = 2;
		ndir.refcount = 1;
		ndir.iops     = &procfs_netdir_iops;
		ndir.fops     = &procfs_dir_fops;
		ndir.cached   = 1;
		init = 1;
	}
	return &ndir;
}

/* ── /proc/sys ───────────────────────────────────────────────── */

typedef struct { const char *name; int type; } procfs_sysfile_t;

static procfs_sysfile_t sys_kernel_files[] = {
	{ "ostype",        PROC_F_SYS_OSTYPE      }, /* uname's sysname, as on Linux */
	{ "hostname",      PROC_F_SYS_HOSTNAME    },
	{ "domainname",    PROC_F_SYS_DOMAINNAME  },
	{ "ngroups_max",   PROC_F_SYS_NGROUPS_MAX },
	{ "pid_max",       PROC_F_SYS_PID_MAX     },
	{ "randomize_va_space", PROC_F_SYS_RANDOMIZE_VA },
	{ "core_pattern",  PROC_F_SYS_CORE_PATTERN },
	{ "kptr_restrict",  PROC_F_SYS_KPTR_RESTRICT  },
	{ "dmesg_restrict", PROC_F_SYS_DMESG_RESTRICT },
	{ "ranix_deadline", PROC_F_SYS_RANIX_DEADLINE },
};
#define SYS_KERNEL_N 10

/* /proc/sys/vm — overcommit_memory lives here in Linux (not under kernel.*) */
static procfs_sysfile_t sys_vm_files[] = {
	{ "max_map_count",     PROC_F_SYS_MAX_MAP_COUNT },
	{ "overcommit_memory", PROC_F_SYS_OVERCOMMIT    },
	{ "swappiness",        PROC_F_SYS_SWAPPINESS    },
};
#define SYS_VM_N 3

static procfs_sysfile_t sys_net_core_files[] = {
	{ "somaxconn",       PROC_F_SYS_SOMAXCONN    },
	{ "rmem_default",    PROC_F_SYS_RMEM_DEFAULT },
	{ "wmem_default",    PROC_F_SYS_WMEM_DEFAULT },
	{ "rmem_max",        PROC_F_SYS_RMEM_MAX     },
	{ "wmem_max",        PROC_F_SYS_WMEM_MAX     },
};
#define SYS_NET_CORE_N 5

static procfs_sysfile_t sys_fs_files[] = {
	{ "file-max",    PROC_F_SYS_FILE_MAX },
	{ "file-nr",     PROC_F_SYS_NFILE    },
	{ "dentry-state", PROC_F_SYS_DENTRY  },
};
#define SYS_FS_N 3

/* forward declarations */
static vfs_inode_t *procfs_sysdir_inode(void);
static vfs_inode_t *procfs_syskerneldir_inode(void);
static vfs_inode_t *procfs_sysnetcoredir_inode(void);
static vfs_inode_t *procfs_sysfsdir_inode(void);
static vfs_inode_t *procfs_sysvmdir_inode(void);
/* generic sysfile inode */
static vfs_inode_t *procfs_sysfile_inode(int type)
{
	vfs_inode_t *in = procfs_inode_file(-1, type);
	if (in && (type == PROC_F_SYS_HOSTNAME ||
	           type == PROC_F_SYS_KPTR_RESTRICT ||
	           type == PROC_F_SYS_DMESG_RESTRICT ||
	           type == PROC_F_SYS_RANIX_DEADLINE))
		in->mode = S_IFREG | 0644;
	return in;
}

/* /proc/sys/kernel */
static vfs_inode_t *procfs_syskerneldir_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	for (int i = 0; i < SYS_KERNEL_N; i++)
		if (strcmp(sys_kernel_files[i].name, name) == 0)
			return procfs_sysfile_inode(sys_kernel_files[i].type);
	return NULL;
}
static int procfs_syskerneldir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= (uint32_t)SYS_KERNEL_N) return -1;
	out->ino = 0xa100 + idx; out->mode = S_IFREG | 0444; out->next_off = idx + 1;
	const char *n = sys_kernel_files[idx].name;
	int nlen = 0; while (n[nlen]) nlen++;
	memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	return 0;
}
static int procfs_syssubdir_stat(vfs_inode_t *in, vfs_stat_t *out)
{
	out->ino = in->ino; out->mode = in->mode; out->nlink = 2; out->size = 0;
	return 0;
}
static vfs_iops_t procfs_syskerneldir_iops = {
	.lookup  = procfs_syskerneldir_lookup,
	.readdir = procfs_syskerneldir_readdir,
	.stat    = procfs_syssubdir_stat,
};
static vfs_inode_t *procfs_syskerneldir_inode(void)
{
	static vfs_inode_t d; static int init = 0;
	if (!init) {
		memset(&d, 0, sizeof(d)); d.ino = 0xa100; d.mode = S_IFDIR | 0555;
		d.nlink = 2; d.refcount = 1; d.iops = &procfs_syskerneldir_iops;
		d.fops = &procfs_dir_fops; d.cached = 1; init = 1;
	}
	return &d;
}

/* /proc/sys/net/core */
static vfs_inode_t *procfs_sysnetcoredir_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	for (int i = 0; i < SYS_NET_CORE_N; i++)
		if (strcmp(sys_net_core_files[i].name, name) == 0)
			return procfs_sysfile_inode(sys_net_core_files[i].type);
	return NULL;
}
static int procfs_sysnetcoredir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= (uint32_t)SYS_NET_CORE_N) return -1;
	out->ino = 0xa200 + idx; out->mode = S_IFREG | 0444; out->next_off = idx + 1;
	const char *n = sys_net_core_files[idx].name;
	int nlen = 0; while (n[nlen]) nlen++;
	memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	return 0;
}
static vfs_iops_t procfs_sysnetcoredir_iops = {
	.lookup  = procfs_sysnetcoredir_lookup,
	.readdir = procfs_sysnetcoredir_readdir,
	.stat    = procfs_syssubdir_stat,
};
static vfs_inode_t *procfs_sysnetcoredir_inode(void)
{
	static vfs_inode_t d; static int init = 0;
	if (!init) {
		memset(&d, 0, sizeof(d)); d.ino = 0xa200; d.mode = S_IFDIR | 0555;
		d.nlink = 2; d.refcount = 1; d.iops = &procfs_sysnetcoredir_iops;
		d.fops = &procfs_dir_fops; d.cached = 1; init = 1;
	}
	return &d;
}

/* /proc/sys/net — contains core/ */
static vfs_inode_t *procfs_sysnetdir_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "core") == 0) return procfs_sysnetcoredir_inode();
	return NULL;
}
static int procfs_sysnetdir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx != 0) return -1;
	out->ino = 0xa210; out->mode = S_IFDIR | 0555; out->next_off = 1;
	out->name[0] = 'c'; out->name[1] = 'o'; out->name[2] = 'r';
	out->name[3] = 'e'; out->name[4] = '\0';
	return 0;
}
static vfs_iops_t procfs_sysnetdir_iops = {
	.lookup  = procfs_sysnetdir_lookup,
	.readdir = procfs_sysnetdir_readdir,
	.stat    = procfs_syssubdir_stat,
};
static vfs_inode_t *procfs_sysnetdir_inode(void)
{
	static vfs_inode_t d; static int init = 0;
	if (!init) {
		memset(&d, 0, sizeof(d)); d.ino = 0xa210; d.mode = S_IFDIR | 0555;
		d.nlink = 2; d.refcount = 1; d.iops = &procfs_sysnetdir_iops;
		d.fops = &procfs_dir_fops; d.cached = 1; init = 1;
	}
	return &d;
}

/* /proc/sys/fs */
static vfs_inode_t *procfs_sysfsdir_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	for (int i = 0; i < SYS_FS_N; i++)
		if (strcmp(sys_fs_files[i].name, name) == 0)
			return procfs_sysfile_inode(sys_fs_files[i].type);
	return NULL;
}
static int procfs_sysfsdir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= (uint32_t)SYS_FS_N) return -1;
	out->ino = 0xa300 + idx; out->mode = S_IFREG | 0444; out->next_off = idx + 1;
	const char *n = sys_fs_files[idx].name;
	int nlen = 0; while (n[nlen]) nlen++;
	memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	return 0;
}
static vfs_iops_t procfs_sysfsdir_iops = {
	.lookup  = procfs_sysfsdir_lookup,
	.readdir = procfs_sysfsdir_readdir,
	.stat    = procfs_syssubdir_stat,
};
static vfs_inode_t *procfs_sysfsdir_inode(void)
{
	static vfs_inode_t d; static int init = 0;
	if (!init) {
		memset(&d, 0, sizeof(d)); d.ino = 0xa300; d.mode = S_IFDIR | 0555;
		d.nlink = 2; d.refcount = 1; d.iops = &procfs_sysfsdir_iops;
		d.fops = &procfs_dir_fops; d.cached = 1; init = 1;
	}
	return &d;
}

/* /proc/sys/vm */
static vfs_inode_t *procfs_sysvmdir_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	for (int i = 0; i < SYS_VM_N; i++)
		if (strcmp(sys_vm_files[i].name, name) == 0)
			return procfs_sysfile_inode(sys_vm_files[i].type);
	return NULL;
}
static int procfs_sysvmdir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	if (idx >= (uint32_t)SYS_VM_N) return -1;
	out->ino = 0xa400 + idx; out->mode = S_IFREG | 0444; out->next_off = idx + 1;
	const char *n = sys_vm_files[idx].name;
	int nlen = 0; while (n[nlen]) nlen++;
	memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	return 0;
}
static vfs_iops_t procfs_sysvmdir_iops = {
	.lookup  = procfs_sysvmdir_lookup,
	.readdir = procfs_sysvmdir_readdir,
	.stat    = procfs_syssubdir_stat,
};
static vfs_inode_t *procfs_sysvmdir_inode(void)
{
	static vfs_inode_t d; static int init = 0;
	if (!init) {
		memset(&d, 0, sizeof(d)); d.ino = 0xa400; d.mode = S_IFDIR | 0555;
		d.nlink = 2; d.refcount = 1; d.iops = &procfs_sysvmdir_iops;
		d.fops = &procfs_dir_fops; d.cached = 1; init = 1;
	}
	return &d;
}

/* /proc/sys root — contains kernel/, net/, fs/, vm/ */
static vfs_inode_t *procfs_sysdir_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;
	if (strcmp(name, "kernel") == 0) return procfs_syskerneldir_inode();
	if (strcmp(name, "net")    == 0) return procfs_sysnetdir_inode();
	if (strcmp(name, "fs")     == 0) return procfs_sysfsdir_inode();
	if (strcmp(name, "vm")     == 0) return procfs_sysvmdir_inode();
	return NULL;
}
static int procfs_sysdir_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	static const char *subdirs[] = { "kernel", "net", "fs", "vm" };
	if (idx >= 4) return -1;
	out->ino = 0xa000 + idx; out->mode = S_IFDIR | 0555; out->next_off = idx + 1;
	const char *n = subdirs[idx];
	int nlen = 0; while (n[nlen]) nlen++;
	memcpy(out->name, n, nlen); out->name[nlen] = '\0';
	return 0;
}
static int procfs_sysdir_stat(vfs_inode_t *in, vfs_stat_t *out)
{
	out->ino = in->ino; out->mode = in->mode; out->nlink = 5; out->size = 0;
	return 0;
}
static vfs_iops_t procfs_sysdir_iops = {
	.lookup  = procfs_sysdir_lookup,
	.readdir = procfs_sysdir_readdir,
	.stat    = procfs_sysdir_stat,
};
static vfs_inode_t *procfs_sysdir_inode(void)
{
	static vfs_inode_t d; static int init = 0;
	if (!init) {
		memset(&d, 0, sizeof(d)); d.ino = 0xa000; d.mode = S_IFDIR | 0555;
		d.nlink = 5; d.refcount = 1; d.iops = &procfs_sysdir_iops;
		d.fops = &procfs_dir_fops; d.cached = 1; init = 1;
	}
	return &d;
}

/* ── root iops ───────────────────────────────────────────────── */

static int procfs_self_readlink(vfs_inode_t *in, char *buf, size_t sz)
{
	(void)in;
	process_t *cur = task_current();
	uint64_t pid = cur ? (uint64_t)task_vpid(cur) : 0;
	/* "N", as on Linux: relative, so it is walked inside this procfs whatever the
	 * reader's root (a /proc fd held from before a chroot reaches self/ too) */
	char tmp[32]; int ti = 0;
	if (pid == 0) { tmp[ti++] = '0'; }
	else {
		char rev[20]; int ri = 0;
		uint64_t v = pid;
		while (v) { rev[ri++] = '0' + (int)(v % 10); v /= 10; }
		for (int j = ri - 1; j >= 0; j--) tmp[ti++] = rev[j];
	}
	tmp[ti] = '\0';
	if (sz < (size_t)ti + 1) return -1;
	memcpy(buf, tmp, (size_t)ti + 1);
	return 0;
}

static vfs_iops_t procfs_self_iops = {
	.readlink = procfs_self_readlink,
};

static vfs_inode_t *procfs_self_inode(void)
{
	static vfs_inode_t self_inode;
	static int init = 0;
	if (!init) {
		memset(&self_inode, 0, sizeof(self_inode));
		self_inode.ino     = 0x7ffe;
		self_inode.mode    = S_IFLNK | 0777;
		self_inode.nlink   = 1;
		self_inode.refcount= 1;
		self_inode.iops    = &procfs_self_iops;
		self_inode.cached  = 1; /* static — must not be freed by vfs_inode_free */
		init = 1;
	}
	return &self_inode;
}

static vfs_inode_t *procfs_root_lookup(vfs_inode_t *dir, const char *name)
{
	(void)dir;

	/* "self" → symlink that resolves to /proc/<current_pid> at traversal time */
	if (strcmp(name, "self") == 0)
		return procfs_self_inode();

	/* "net" subdirectory */
	if (strcmp(name, "net") == 0)
		return procfs_netdir_inode();

	/* "sys" subdirectory */
	if (strcmp(name, "sys") == 0)
		return procfs_sysdir_inode();

	if (strcmp(name, "asess") == 0)
		return procfs_asessroot_inode();

	/* static files */
	for (int i = 0; i < ROOT_FILES_N; i++)
		if (strcmp(root_files[i].name, name) == 0)
			return procfs_inode_file(-1, root_files[i].type);

	/* numeric PID dirs: the name is a pid as seen in the caller's pid namespace;
	 * translate it to the global task (and reject pids not visible in this ns). */
	int vnr = 0; const char *p = name;
	while (*p >= '0' && *p <= '9') { vnr = vnr * 10 + (*p - '0'); p++; }
	if (*p == '\0' && p != name) {
		process_t *tp = pidns_find(current_pid_ns(), vnr);
		if (!tp) return NULL;
		return procfs_piddir_inode((int)tp->pid);
	}

	return NULL;
}

static int procfs_root_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	(void)dir;
	int n_proc = task_count();

	/* static files first */
	if (idx < ROOT_FILES_N) {
		out->ino      = 0x8000 + idx;
		out->mode     = S_IFREG | 0444;
		out->next_off = idx + 1;
		const char *n = root_files[idx].name;
		int nlen = 0; while (n[nlen]) nlen++;
		memcpy(out->name, n, nlen); out->name[nlen] = '\0';
		return 0;
	}
	idx -= ROOT_FILES_N;

	/* "self" entry */
	if (idx == 0) {
		out->ino      = 0x7ffe;
		out->mode     = S_IFLNK | 0777;
		out->next_off = ROOT_FILES_N + 1;
		out->name[0] = 's'; out->name[1] = 'e'; out->name[2] = 'l';
		out->name[3] = 'f'; out->name[4] = '\0';
		return 0;
	}
	idx--;

	/* "net" dir entry */
	if (idx == 0) {
		out->ino      = 0x9000;
		out->mode     = S_IFDIR | 0555;
		out->next_off = ROOT_FILES_N + 2;
		out->name[0] = 'n'; out->name[1] = 'e'; out->name[2] = 't';
		out->name[3] = '\0';
		return 0;
	}
	idx--;

	/* "sys" dir entry */
	if (idx == 0) {
		out->ino      = 0xa000;
		out->mode     = S_IFDIR | 0555;
		out->next_off = ROOT_FILES_N + 3;
		out->name[0] = 's'; out->name[1] = 'y'; out->name[2] = 's';
		out->name[3] = '\0';
		return 0;
	}
	idx--;

	if (idx == 0) {
		out->ino      = 0xb000;
		out->mode     = S_IFDIR | 0555;
		out->next_off = ROOT_FILES_N + 4;
		memcpy(out->name, "asess", 6);
		return 0;
	}
	idx--;

	/* per-process dirs: the idx-th process visible in the caller's pid namespace,
	 * named by its pid as seen in that namespace (vnr). */
	pid_namespace_t *cur_ns = current_pid_ns();
	/* idx is the cookie of the last call: a place in the task table, so a
	 * listing is one pass and not one pass per entry */
	for (int i = (int)idx; i < n_proc; i++) {
		process_t *p = task_get(i);
		if (!p) continue;
		if (p->tgid && p->tgid != p->pid) continue;  /* threads: /proc/<pid>/task */
		int vnr = task_pid_nr_ns(p, cur_ns);
		if (vnr <= 0) {
			if (cur_ns != &init_pid_ns) continue;   /* not visible in this ns */
			vnr = (int)p->pid;                      /* root ns sees every task */
		}
		{
			out->ino      = 0x8000 + (uint64_t)vnr * 100 + 50;
			out->mode     = S_IFDIR | 0555;
			out->next_off = ROOT_FILES_N + 4 + (uint32_t)i + 1;
			char tmp[16]; int ti = 0;
			uint32_t v = (uint32_t)vnr;
			if (v == 0) { tmp[ti++] = '0'; }
			else { char rev[10]; int ri = 0; while (v) { rev[ri++] = '0' + (v % 10); v /= 10; } for (int j = ri-1; j >= 0; j--) tmp[ti++] = rev[j]; }
			memcpy(out->name, tmp, ti); out->name[ti] = '\0';
			return 0;
		}
	}
	return -1;
}

static int procfs_root_stat(vfs_inode_t *in, vfs_stat_t *out)
{
	out->ino   = in->ino;
	out->mode  = in->mode;
	out->nlink = in->nlink;
	out->size  = 0;
	return 0;
}

static vfs_iops_t procfs_root_iops = {
	.lookup  = procfs_root_lookup,
	.readdir = procfs_root_readdir,
	.stat    = procfs_root_stat,
};

/* The /proc of an agent session: pid directories of its namespace, and only the
 * host-wide files a program needs in order to run. The kernel command line and
 * the disk, module and interrupt tables stay out. */
static int procfs_session_allows(const char *name)
{
	static const char *const allowed[] = {
		"self", "sys", "version", "cpuinfo", "meminfo", "mounts", "uptime", "stat",
		"loadavg", "filesystems",
	};
	if (name[0] >= '0' && name[0] <= '9') return 1;
	for (unsigned i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++)
		if (strcmp(allowed[i], name) == 0) return 1;
	return 0;
}

static vfs_inode_t *procfs_session_lookup(vfs_inode_t *dir, const char *name)
{
	return procfs_session_allows(name) ? procfs_root_lookup(dir, name) : NULL;
}

static int procfs_session_readdir(vfs_inode_t *dir, uint32_t idx, vfs_dirent_t *out)
{
	/* the root's cookies, filtered: the next call goes on where this one stopped */
	for (uint32_t i = idx; procfs_root_readdir(dir, i, out) == 0; i = out->next_off)
		if (procfs_session_allows(out->name))
			return 0;
	return -1;
}

static vfs_iops_t procfs_session_iops = {
	.lookup  = procfs_session_lookup,
	.readdir = procfs_session_readdir,
	.stat    = procfs_root_stat,
};

/* ── mount ───────────────────────────────────────────────── */

int procfs_mount(vfs_mount_t *mnt, void *data)
{
	vfs_inode_t *root_inode = kmalloc(sizeof(*root_inode));
	if (!root_inode) return -1;
	memset(root_inode, 0, sizeof(*root_inode));
	root_inode->ino      = 1;
	root_inode->mode     = S_IFDIR | 0555;
	root_inode->nlink    = 2;
	root_inode->refcount = 1;
	root_inode->iops     = data && strcmp(data, ASESS_MOUNT_OPT) == 0 ? &procfs_session_iops
	                                                                  : &procfs_root_iops;
	root_inode->fops     = &procfs_dir_fops;
	root_inode->no_cache = 1; /* PID dirs are dynamic — never cache children */

	vfs_dentry_t *root_dentry = vfs_dentry_create(NULL, "/", root_inode);
	if (!root_dentry) { kfree(root_inode); return -1; }

	mnt->root = root_dentry;
	return vfs_simple_sb(mnt, NULL);
}

void procfs_register(void)
{
	vfs_register_fs("proc", procfs_mount);
}
