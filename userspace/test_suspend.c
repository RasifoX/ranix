/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The guest half of tests/suspend.py (include/kernel/sleep.h).
 *
 *   test_suspend files [deep]
 *       /sys/power/state is "freeze mem"; mem_sleep is "[s2idle]" ("[s2idle] deep"
 *       with the argument); a user that is not root cannot write state; deep is
 *       refused unless offered. Last line "SUS-FILES OK" or "SUS-FILES FAIL <why>".
 *
 *   test_suspend sleep freeze|mem SECS
 *       A watcher on the last CPU reads CLOCK_MONOTONIC through the vDSO the whole
 *       time and counts steps back; a child naps 3 s across the sleep. SECS > 0
 *       sets the RTC wake alarm (+SECS); 0 leaves the waking to the test (button,
 *       key, system_wakeup). Prints "SUS-READY", sleeps, then
 *       "SUS-RESULT err=.. mono_ms=.. boot_ms=.. real_ms=.. dv_mono_us=.. dv_boot_us=..
 *        dv_real_us=.. back=.. after=.. last=<ranix_last> success=.."
 *       and "SUS-NAP mono_ms=.. boot_ms=..". The host judges the numbers.
 *
 *   test_suspend tail freeze|mem SECS
 *       The early-wake tests across a sleep (test_sleeptail, test_vvarepoch):
 *       on the last CPU, 2.5 ms MONOTONIC sleeps one after another, before,
 *       across and after the suspend, with the vvar periods watched as
 *       test_vvarepoch does. Prints "SUS-TAIL n=.. early=.. late=.. cross_over_us=..
 *       mono_gap_ms=.. boot_gap_ms=.. vvar_n=.. vvar_bad=.. vvar_skip=..": no sleep
 *       may end early, the one across the suspend included (MONOTONIC stood
 *       still), MONOTONIC must not jump while BOOTTIME took the whole sleep, and
 *       every vvar period continues the one before it to 1 ns, except across the
 *       suspend (the counter was re-anchored there: skipped and counted).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../include/uapi/vdso.h"   /* struct vdso_vvar, VVAR_VADDR */

static int64_t ns_of(struct timespec t) { return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }

static int64_t vclock(int c) { struct timespec t; clock_gettime(c, &t); return ns_of(t); }
static int64_t sclock(int c) { struct timespec t; syscall(SYS_clock_gettime, c, &t); return ns_of(t); }

static int first_line(const char *p, char *b, size_t n)
{
	int fd = open(p, O_RDONLY);
	if (fd < 0) return -1;
	ssize_t k = read(fd, b, n - 1);
	close(fd);
	if (k < 0) return -1;
	b[k] = 0;
	b[strcspn(b, "\n")] = 0;
	return 0;
}

static int put(const char *p, const char *s)
{
	int fd = open(p, O_WRONLY);
	if (fd < 0) return -errno;
	ssize_t k = write(fd, s, strlen(s));
	int e = k < 0 ? -errno : 0;
	close(fd);
	return e;
}

static int files(int deep)
{
	char b[128], why[200] = "";
	if (first_line("/sys/power/state", b, sizeof b) || strcmp(b, "freeze mem"))
		snprintf(why, sizeof why, "state '%s'", b);
	else if (first_line("/sys/power/mem_sleep", b, sizeof b) || strcmp(b, deep ? "[s2idle] deep" : "[s2idle]"))
		snprintf(why, sizeof why, "mem_sleep '%s'", b);
	else if (first_line("/sys/power/suspend_stats/success", b, sizeof b) || strcmp(b, "0"))
		snprintf(why, sizeof why, "success '%s'", b);
	else if (first_line("/sys/class/rtc/rtc0/name", b, sizeof b) || strcmp(b, "rtc_cmos"))
		snprintf(why, sizeof why, "rtc0 name '%s'", b);
	else if (!deep && put("/sys/power/mem_sleep", "deep\n") != -EINVAL)
		snprintf(why, sizeof why, "deep not refused");
	else if (put("/sys/power/state", "disk\n") != -EINVAL)
		snprintf(why, sizeof why, "disk not refused");
	else if (put("/sys/class/rtc/rtc0/wakealarm", "5\n") != -EINVAL)
		snprintf(why, sizeof why, "a wakealarm in the past not refused");
	else {
		pid_t c = fork();
		if (c == 0) {
			if (setgid(65534) || setuid(65534)) _exit(3);
			int e = put("/sys/power/state", "freeze\n");
			_exit(e == -EPERM || e == -EACCES ? 0 : 1);
		}
		int st = 0;
		waitpid(c, &st, 0);
		if (!WIFEXITED(st) || WEXITSTATUS(st))
			snprintf(why, sizeof why, "a user could write state (%d)", WEXITSTATUS(st));
	}
	printf("SUS-FILES %s%s\n", *why ? "FAIL " : "OK", why);
	return *why ? 1 : 0;
}

struct watch { volatile int stop, resumed; volatile int64_t back, max_back_ns, after, back_t; };

static void watcher(struct watch *w, int cpu)
{
	cpu_set_t s;
	CPU_ZERO(&s);
	CPU_SET(cpu, &s);
	sched_setaffinity(0, sizeof s, &s);
	int64_t last = vclock(CLOCK_MONOTONIC);
	while (!w->stop) {
		int64_t t = vclock(CLOCK_MONOTONIC);
		if (t < last) { w->back++; if (last - t > w->max_back_ns) { w->max_back_ns = last - t; w->back_t = t; } }
		if (w->resumed) w->after++;
		last = t;
	}
	_exit(0);
}

static int sleep_test(const char *kind, int secs)
{
	struct watch *w = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (w == MAP_FAILED) { printf("SUS-RESULT err=mmap\n"); return 1; }
	memset((void *)w, 0, sizeof *w);
	int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
	pid_t wp = fork();
	if (wp == 0) watcher(w, ncpu > 1 ? ncpu - 1 : 0);

	int pfd[2];
	if (pipe(pfd)) return 1;
	pid_t nap = fork();
	if (nap == 0) {
		int64_t m0 = sclock(CLOCK_MONOTONIC), b0 = sclock(CLOCK_BOOTTIME);
		struct timespec d = { 3, 0 };
		while (nanosleep(&d, &d) && errno == EINTR) { }
		char o[96];
		int n = snprintf(o, sizeof o, "SUS-NAP mono_ms=%lld boot_ms=%lld\n",
		                 (long long)((sclock(CLOCK_MONOTONIC) - m0) / 1000000),
		                 (long long)((sclock(CLOCK_BOOTTIME) - b0) / 1000000));
		if (write(pfd[1], o, (size_t)n) < 0) { }
		_exit(0);
	}
	close(pfd[1]);
	usleep(300000);

	char b[160];
	if (secs > 0) {
		snprintf(b, sizeof b, "+%d\n", secs);
		int e = put("/sys/class/rtc/rtc0/wakealarm", b);
		if (e) printf("SUS-NOTE wakealarm %d\n", e);
	}
	int64_t m0 = sclock(CLOCK_MONOTONIC), bt0 = sclock(CLOCK_BOOTTIME), r0 = sclock(CLOCK_REALTIME);
	printf("SUS-READY real=%lld\n", (long long)(r0 / 1000000000LL));
	fflush(stdout);
	char k[16];
	snprintf(k, sizeof k, "%s\n", kind);
	int err = put("/sys/power/state", k);
	int64_t m1 = sclock(CLOCK_MONOTONIC), bt1 = sclock(CLOCK_BOOTTIME), r1 = sclock(CLOCK_REALTIME);
	w->resumed = 1;
	/* the vDSO against the system call, each clock read vDSO-syscall-vDSO */
	int64_t dv[3];
	int clk[3] = { CLOCK_MONOTONIC, CLOCK_BOOTTIME, CLOCK_REALTIME };
	for (int i = 0; i < 3; i++) {
		int64_t a = vclock(clk[i]), s = sclock(clk[i]), c = vclock(clk[i]);
		dv[i] = s < a ? a - s : s > c ? s - c : 0;     /* outside [a, c]: by how much */
	}
	usleep(200000);
	w->stop = 1;
	waitpid(wp, 0, 0);
	char last[96] = "", ok[16] = "";
	first_line("/sys/power/ranix_last", last, sizeof last);
	first_line("/sys/power/suspend_stats/success", ok, sizeof ok);
	for (char *p = last; *p; p++) if (*p == ' ') *p = ',';
	printf("SUS-RESULT err=%d mono_ms=%lld boot_ms=%lld real_ms=%lld dv_mono_us=%lld dv_boot_us=%lld dv_real_us=%lld "
	       "back=%lld max_back_us=%lld after=%lld last=%s success=%s real_end=%lld back_ns=%lld back_at_us=%lld\n", err,
	       (long long)((m1 - m0) / 1000000), (long long)((bt1 - bt0) / 1000000), (long long)((r1 - r0) / 1000000),
	       (long long)(dv[0] / 1000), (long long)(dv[1] / 1000), (long long)(dv[2] / 1000),
	       (long long)w->back, (long long)(w->max_back_ns / 1000), (long long)w->after, last, ok,
	       (long long)(r1 / 1000000000LL), (long long)w->max_back_ns,
	       (long long)(w->back ? (w->back_t - m0) / 1000 : 0));   /* where in MONOTONIC: 0 = the write, mono_ms = its return */
	fflush(stdout);
	waitpid(nap, 0, 0);
	ssize_t n = read(pfd[0], b, sizeof b - 1);
	if (n > 0) { b[n] = 0; fputs(b, stdout); }
	fflush(stdout);
	return err ? 1 : 0;
}

struct tail {
	volatile int stop;
	volatile long long n, early, late, cross_over_ns, mono_gap_ns, boot_gap_ns;
	volatile long long vn, vbad, vskip;
};

/* the 2.5 ms sleeps and the vvar watch, on one CPU until stop */
static void tailer(struct tail *r, int cpu)
{
	cpu_set_t s;
	CPU_ZERO(&s);
	CPU_SET(cpu, &s);
	sched_setaffinity(0, sizeof s, &s);
	const volatile struct vdso_vvar *v = (const volatile struct vdso_vvar *)VVAR_VADDR;
	unsigned oseq = 0;
	unsigned long long ob = 0;
	long long om = 0, obo = 0;
	int ovalid = 0;
	while (!r->stop) {
		struct timespec d = { 0, 2500000 };
		int64_t a = vclock(CLOCK_MONOTONIC), ab = vclock(CLOCK_BOOTTIME);
		clock_nanosleep(CLOCK_MONOTONIC, 0, &d, NULL);
		int64_t z = vclock(CLOCK_MONOTONIC), zb = vclock(CLOCK_BOOTTIME);
		int64_t over = z - a - 2500000, bgap = zb - ab;
		r->n++;
		if (over < 0) r->early++;
		if (bgap > r->boot_gap_ns) { r->boot_gap_ns = bgap; r->cross_over_ns = over; }   /* the sleep across the suspend */
		else if (over > 300000) r->late++;
		if (z - a > r->mono_gap_ns) r->mono_gap_ns = z - a;
		/* test_vvarepoch's check on the period now in force */
		unsigned q = v->seq;
		if ((q & 1) || q == oseq) continue;
		__asm__ volatile("" ::: "memory");
		unsigned long long b = v->cnt_base;
		long long m = v->mono_sec * 1000000000LL + v->mono_nsec;
#ifdef VDSO_VVAR_BOOT_OFF
		long long bo = v->boot_off_sec * 1000000000LL + v->boot_off_nsec;
#else
		long long bo = 0;   /* older kernel: no suspend, nothing re-anchors (the run fails on /sys/power first) */
#endif
		unsigned mult = v->cnt_mult, sh = v->cnt_shift;
		int valid = (int)v->cnt_valid;
		__asm__ volatile("" ::: "memory");
		if (v->seq != q) continue;
		if (oseq && valid && ovalid) {
			if (bo != obo || b < ob) r->vskip++;      /* a suspend between: re-anchored */
			else {
				long long j = (m - om) - (long long)(((b - ob) * (unsigned long long)mult) >> sh);
				r->vn++;
				if (j < 0 || j > 1) r->vbad++;
			}
		} else if (oseq) r->vskip++;
		oseq = q; ob = b; om = m; obo = bo; ovalid = valid;
	}
	_exit(0);
}

static int tail_test(const char *kind, int secs)
{
	struct tail *r = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (r == MAP_FAILED) { printf("SUS-TAIL err=mmap\n"); return 1; }
	memset((void *)r, 0, sizeof *r);
	int ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
	pid_t c = fork();
	if (c == 0) tailer(r, ncpu > 1 ? ncpu - 1 : 0);
	sleep(1);
	char b[32];
	snprintf(b, sizeof b, "+%d\n", secs);
	if (secs > 0 && put("/sys/class/rtc/rtc0/wakealarm", b)) printf("SUS-NOTE wakealarm failed\n");
	printf("SUS-READY tail\n");
	fflush(stdout);
	snprintf(b, sizeof b, "%s\n", kind);
	int err = put("/sys/power/state", b);
	sleep(2);
	r->stop = 1;
	waitpid(c, 0, 0);
	printf("SUS-TAIL err=%d n=%lld early=%lld late=%lld cross_over_us=%lld mono_gap_ms=%lld boot_gap_ms=%lld "
	       "vvar_n=%lld vvar_bad=%lld vvar_skip=%lld\n", err, r->n, r->early, r->late, r->cross_over_ns / 1000,
	       r->mono_gap_ns / 1000000, r->boot_gap_ns / 1000000, r->vn, r->vbad, r->vskip);
	return err || r->early ? 1 : 0;
}

int main(int argc, char **argv)
{
	setvbuf(stdout, 0, _IOLBF, 0);
	if (argc >= 2 && !strcmp(argv[1], "files")) return files(argc >= 3 && !strcmp(argv[2], "deep"));
	if (argc >= 4 && !strcmp(argv[1], "sleep")) return sleep_test(argv[2], atoi(argv[3]));
	if (argc >= 4 && !strcmp(argv[1], "tail")) return tail_test(argv[2], atoi(argv[3]));
	fprintf(stderr, "usage: test_suspend files [deep] | sleep freeze|mem SECS\n");
	return 2;
}
