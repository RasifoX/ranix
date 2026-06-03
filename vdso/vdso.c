/* Ranix vDSO — user-space fast paths for clock_gettime/gettimeofday that read
 * the kernel's shared vvar page instead of trapping. Built as a freestanding
 * PIC shared object; the kernel maps it (plus the vvar one page below it) into
 * every process and points AT_SYSINFO_EHDR at it. libc parses the ELF and calls
 * the symbols by name+version: x86_64 wants __vdso_* under LINUX_2.6, aarch64
 * wants __kernel_* under LINUX_2.6.39. Unsupported clocks fall back to the real
 * syscall from inside the vDSO (as glibc's does), so the caller always gets an
 * answer. */
#include <stdint.h>
#include <uapi/vdso.h>

struct vts { long tv_sec; long tv_nsec; };
struct vtv { long tv_sec; long tv_usec; };

#if defined(__x86_64__)
#define VNAME(f)  __vdso_##f
#define SYS_clock_gettime_   228
#define SYS_gettimeofday_    96
#define SYS_clock_getres_    229
#else
#define VNAME(f)  __kernel_##f
#define SYS_clock_gettime_   113
#define SYS_gettimeofday_    169
#define SYS_clock_getres_    114
#endif

/* The kernel maps vvar at the fixed VVAR_VADDR; an absolute constant needs no
 * relocation, so the PIC vDSO can read it directly. */
#define VVAR ((const volatile struct vdso_vvar *)VVAR_VADDR)

static inline uint64_t read_cnt(void)
{
#if defined(__aarch64__)
	uint64_t c;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(c));
	return c;
#elif defined(__x86_64__)
	uint32_t lo, hi;
	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return ((uint64_t)hi << 32) | lo;
#else
	return 0;
#endif
}

static inline long vsyscall2(long n, long a, long b)
{
#if defined(__aarch64__)
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory", "cc");
	return x0;
#elif defined(__x86_64__)
	long ret;
	register long r10 __asm__("r10");
	(void)r10;
	__asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b)
	                 : "rcx", "r11", "memory");
	return ret;
#else
	(void)n; (void)a; (void)b; return -38;
#endif
}

int VNAME(clock_gettime)(int clk, struct vts *ts)
{
	const volatile struct vdso_vvar *v = VVAR;
	if (clk != 0 /*REALTIME*/ && clk != 1 /*MONOTONIC*/)
		return (int)vsyscall2(SYS_clock_gettime_, clk, (long)ts);
	for (;;) {
		uint32_t s = v->seq;
		if (s & 1u) continue;
		__asm__ volatile("" ::: "memory");
		int64_t sec  = (clk == 0) ? v->real_sec  : v->mono_sec;
		int64_t nsec = (clk == 0) ? v->real_nsec : v->mono_nsec;
		if (v->cnt_valid) {
			uint64_t delta = read_cnt() - v->cnt_base;
			nsec += (int64_t)((delta * v->cnt_mult) >> v->cnt_shift);
		}
		__asm__ volatile("" ::: "memory");
		if (v->seq != s) continue;
		sec  += nsec / 1000000000;
		nsec %= 1000000000;
		ts->tv_sec = sec;
		ts->tv_nsec = nsec;
		return 0;
	}
}

int VNAME(gettimeofday)(struct vtv *tv, void *tz)
{
	if (tz) return (int)vsyscall2(SYS_gettimeofday_, (long)tv, (long)tz);
	struct vts ts;
	if (VNAME(clock_gettime)(0, &ts) != 0)
		return (int)vsyscall2(SYS_gettimeofday_, (long)tv, (long)tz);
	if (tv) { tv->tv_sec = ts.tv_sec; tv->tv_usec = ts.tv_nsec / 1000; }
	return 0;
}

int VNAME(clock_getres)(int clk, struct vts *ts)
{
	return (int)vsyscall2(SYS_clock_getres_, clk, (long)ts);
}

#if defined(__x86_64__)
long __vdso_time(long *tp)
{
	struct vts ts;
	if (__vdso_clock_gettime(0, &ts) != 0) {
		register long r10 __asm__("r10"); (void)r10;
		long ret;
		__asm__ volatile("syscall" : "=a"(ret) : "a"(201L), "D"((long)tp) : "rcx","r11","memory");
		return ret;
	}
	if (tp) *tp = ts.tv_sec;
	return ts.tv_sec;
}

int __vdso_getcpu(unsigned *cpu, unsigned *node, void *unused)
{
	(void)unused;
	if (cpu)  *cpu = 0;
	if (node) *node = 0;
	return 0;
}
#endif
