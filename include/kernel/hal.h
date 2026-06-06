#ifndef _KERNEL_HAL_H
#define _KERNEL_HAL_H

#include <asm/context.h>

#if defined(__aarch64__)
#define smp_mb()  __asm__ volatile("dsb ish" ::: "memory")
#define smp_rmb() __asm__ volatile("dsb ishld" ::: "memory")
#define smp_wmb() __asm__ volatile("dsb ishst" ::: "memory")
#define arch_flush_icache() __asm__ volatile("dsb ish; ic iallu; dsb ish; isb" ::: "memory")
#define cpu_relax() __asm__ volatile("yield" ::: "memory")
#elif defined(__x86_64__)
#define smp_mb()  __asm__ volatile("mfence" ::: "memory")
#define smp_rmb() __asm__ volatile("lfence" ::: "memory")
#define smp_wmb() __asm__ volatile("sfence" ::: "memory")
#define arch_flush_icache() __asm__ volatile("" ::: "memory")
#define cpu_relax() __asm__ volatile("pause" ::: "memory")
#else
#define smp_mb()  __asm__ volatile("" ::: "memory")
#define smp_rmb() __asm__ volatile("" ::: "memory")
#define smp_wmb() __asm__ volatile("" ::: "memory")
#define arch_flush_icache() __asm__ volatile("" ::: "memory")
#define cpu_relax() __asm__ volatile("" ::: "memory")
#endif

void arch_halt(void);
void arch_yield(void);
void arch_cpu_init(void);

/* Initialize a fresh CPU context for a kernel task. */
void arch_context_init(arch_context_t *ctx, void (*entry)(void), uint64_t stack_top);

/* Initialize a CPU context that will drop to unprivileged mode (EL0 / ring3). */
void arch_context_init_user(arch_context_t *ctx, uint64_t entry, uint64_t ustack_top);

/* CPU-specific data area */
typedef struct cpu_context {
	struct cpu_context *self;       /* offset  0 */
	struct process     *current;    /* offset  8 */
	uint8_t            *kstack_top; /* offset 16 — current process kstack */
	uint8_t            *irq_stack;  /* offset 24 — dedicated IRQ stack */
	uint64_t            cpu_id;     /* offset 32 */
	uint64_t            magic;      /* offset 40 */
	uint64_t            scratch;    /* offset 48 — entry path scratch */
	/* SMP fields (offset >= 56 — never touched by assembly) */
	struct process     *rq_head;    /* offset 56 — per-CPU run queue */
	struct process     *rq_tail;    /* offset 64 */
	volatile uint32_t   rq_lock;    /* offset 72 */
	volatile int        online;     /* offset 76 — 1 when CPU is active */
	volatile int        need_resched; /* offset 80 — IPI reschedule flag */
	int                 _pad;       /* offset 84 */
	/* offset 88+ — safe from assembly */
	struct process     *idle;           /* offset 88 — per-CPU idle process */
	uint8_t            *dying_kstack;   /* offset 96 — kstack deferred free after stack switch */
	int                 dying_kstack_order; /* offset 104 */
	int                 _pad2;          /* offset 108 */
	struct process     *pending_requeue; /* offset 112 — prev to add to runqueue after stack switch */
	/* watchdog (C-only) — wd_ticks frozen ⇒ this CPU is wedged with IRQs off */
	volatile uint64_t   wd_ticks;       /* bumped every local timer tick */
	volatile const char *wd_where;      /* breadcrumb: last kernel region entered */
} cpu_context_t;

/* Get the current CPU's context structure. Implemented in arch/ */
cpu_context_t *arch_get_cpu_ctx(void);

/* SMP: bring secondary CPU online (arch-specific boot sequence) */
void arch_smp_boot_cpu(int cpu_id);
/* SMP: send reschedule IPI to cpu_id */
void arch_smp_ipi_send(int cpu_id);
/* SMP: acknowledge and clear IPI (called inside IPI handler) */
void arch_smp_ipi_ack(void);
/* SMP: secondary CPU arch-level init (called from secondary trampoline) */
void arch_secondary_init(int cpu_id);

/* Update the kernel stack pointer used on privilege transitions (TSS.RSP0 / SP_EL1). */
void arch_set_user_kstack(uint64_t kstack_top);

/* TLS base register save/restore (FS_BASE on x86_64, TPIDR_EL0 on AArch64) */
struct process;
void arch_save_tls(struct process *p);
void arch_restore_tls(struct process *p);

/* SMAP/PAN enforcement is OFF this phase: the two arch hooks below are no-ops.
 * Every user touch is already routed through them so a later enforcement phase
 * flips the whole tree on by editing only these two lines (x86: stac/clac,
 * aarch64: msr PSTATE.PAN clear/set). */
#if defined(__x86_64__)
static inline void arch_user_access_begin(void) { __asm__ volatile("" ::: "memory"); }
static inline void arch_user_access_end(void)   { __asm__ volatile("" ::: "memory"); }
#else
static inline void arch_user_access_begin(void) { __asm__ volatile("" ::: "memory"); }
static inline void arch_user_access_end(void)   { __asm__ volatile("" ::: "memory"); }
#endif

/* Bracket helpers for the bulk and user-stack cases (signal frame, ptrace
 * peek/poke) that cannot use copy_*_user. No-ops until enforcement; the single
 * funnel point is the two arch hooks above. */
static inline void uaccess_begin(void) { arch_user_access_begin(); }
static inline void uaccess_end(void)   { arch_user_access_end(); }

/* Safe user memory access */
int  access_ok(const void *addr, size_t n);
int  user_pages_mapped(const void *addr, size_t n);
long copy_from_user(void *dst, const void *src, size_t n);
long copy_to_user(void *dst, const void *src, size_t n);
long strncpy_from_user(char *dst, const char *src, size_t n);

/* Single-scalar accessors built on the same extable-protected primitive as
 * copy_*_user. Return 0 on success, -1 on bad address/fault. */
long __get_user_n(void *dst, const void *src, size_t n);
long __put_user_n(void *dst, const void *src, size_t n);
#define get_user(x, ptr) \
	__get_user_n(&(x), (ptr), sizeof(*(ptr)))
#define put_user(x, ptr) \
	({ __typeof__(*(ptr)) __pu_v = (x); __put_user_n((ptr), &__pu_v, sizeof(*(ptr))); })

/* Fop read/write funnel. A vfs fops ->read/->write buffer may be a user pointer
 * (the read(2)/write(2) data path) OR a kernel pointer (vfs_kernel_install,
 * file-backed mmap page fill, overlay copy-up, io_uring). Route user addresses
 * through the extable-protected copy; fall back to memcpy for kernel addresses.
 * Behavior-preserving with SMAP/PAN off. Returns 0 on success, -1 on fault. */
long copy_fop_to_user(void *dst, const void *src, size_t n);
long copy_fop_from_user(void *dst, const void *src, size_t n);

/* Register access abstractions */
static inline uint32_t reg_read32(uint64_t addr) {
	return *(volatile uint32_t *)addr;
}
static inline void reg_write32(uint64_t addr, uint32_t val) {
	*(volatile uint32_t *)addr = val;
}

#endif
