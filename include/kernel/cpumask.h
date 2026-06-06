#ifndef _KERNEL_CPUMASK_H
#define _KERNEL_CPUMASK_H

#include <uapi/types.h>
#include <kernel/config.h>
#include <kernel/lib/string.h>

/* Compile-time ceiling on logical CPU ids, Linux's NR_CPUS. cpumask_t carries a
 * bit per possible CPU; bring-up (FDT/MADT) is separately clamped to MAX_CPUS.
 * Detection beyond NR_CPUS is impossible, so any id handed to a mask op is in
 * range. At 8192 a cpumask is 1 KB — never embedded in a process; affinity is a
 * lazily-allocated pointer (NULL = all CPUs). */
#define NR_CPUS CONFIG_NR_CPUS

#define BITS_PER_LONG 64
#define CPUMASK_LONGS ((NR_CPUS + BITS_PER_LONG - 1) / BITS_PER_LONG)

typedef struct cpumask { uint64_t bits[CPUMASK_LONGS]; } cpumask_t;

_Static_assert(NR_CPUS >= 1, "NR_CPUS must be positive");
_Static_assert(CPUMASK_LONGS * BITS_PER_LONG >= NR_CPUS, "cpumask too small");

static inline void cpumask_clear(cpumask_t *m) { memset(m, 0, sizeof(*m)); }

static inline void cpumask_set_cpu(int cpu, cpumask_t *m)
{
	m->bits[(unsigned)cpu / BITS_PER_LONG] |= 1ULL << ((unsigned)cpu % BITS_PER_LONG);
}

static inline void cpumask_clear_cpu(int cpu, cpumask_t *m)
{
	m->bits[(unsigned)cpu / BITS_PER_LONG] &= ~(1ULL << ((unsigned)cpu % BITS_PER_LONG));
}

static inline int cpumask_test_cpu(int cpu, const cpumask_t *m)
{
	return (m->bits[(unsigned)cpu / BITS_PER_LONG] >> ((unsigned)cpu % BITS_PER_LONG)) & 1;
}

static inline int cpumask_empty(const cpumask_t *m)
{
	for (int i = 0; i < CPUMASK_LONGS; i++)
		if (m->bits[i]) return 0;
	return 1;
}

static inline int cpumask_weight(const cpumask_t *m)
{
	int w = 0;
	for (int i = 0; i < CPUMASK_LONGS; i++)
		w += __builtin_popcountll(m->bits[i]);
	return w;
}

/* First set bit >= start, or NR_CPUS if none. */
static inline int cpumask_next(int start, const cpumask_t *m)
{
	for (int cpu = start; cpu < NR_CPUS; ) {
		int w = cpu / BITS_PER_LONG;
		uint64_t v = m->bits[w] >> (cpu % BITS_PER_LONG);
		if (v) return cpu + __builtin_ctzll(v);
		cpu = (w + 1) * BITS_PER_LONG;
	}
	return NR_CPUS;
}

static inline int cpumask_first(const cpumask_t *m) { return cpumask_next(0, m); }

#define for_each_cpu(cpu, mask) \
	for ((cpu) = cpumask_first(mask); (cpu) < NR_CPUS; (cpu) = cpumask_next((cpu) + 1, mask))

/* Fill bits [0, n) — the "online up to n CPUs" pattern. */
static inline void cpumask_set_first_n(int n, cpumask_t *m)
{
	cpumask_clear(m);
	for (int i = 0; i < n && i < NR_CPUS; i++) cpumask_set_cpu(i, m);
}

#endif
