/*
 * FP/SIMD context-switch integrity under SMP migration.
 *
 * Each worker runs a tight loop of SIMD-heavy float math whose result is a
 * deterministic constant, and asserts that constant on every iteration. Many
 * more workers than CPUs are spawned so they preempt and migrate constantly;
 * affinity is churned to force cross-CPU moves. If the kernel fails to save or
 * restore a thread's FP/SIMD register file across a context switch or
 * migration, the running accumulator diverges and the worker reports MISMATCH.
 *
 * Compiled -O2 so the compiler vectorises the kernel (SSE/XMM on x86, NEON on
 * aarch64); each worker keeps a wide live FP working set across the loop so a
 * lost q-register/XMM corrupts the result rather than being recomputed.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define NWORK   16        /* >> typical -smp 4 to force migration */
#define WIDTH   64        /* live FP lanes carried across the loop */
#define INNER   20000
#define ROUNDS  400

static volatile int g_fail;
static volatile int g_stop;

/* Deterministic per-seed reduction over a wide FP working set. Returns a value
 * that depends on the full WIDTH-lane state surviving every context switch. */
static double kernel_round(double seed)
{
    double a[WIDTH];
    for (int i = 0; i < WIDTH; i++)
        a[i] = seed + (double)i * 0.5;

    for (int k = 0; k < INNER; k++) {
        for (int i = 0; i < WIDTH; i++) {
            /* fused-ish multiply/add chain, no overflow, fully reproducible */
            a[i] = a[i] * 1.0000001 + 0.5;
            a[i] = a[i] - (a[i] * 0.0000001);
        }
    }

    double s = 0.0;
    for (int i = 0; i < WIDTH; i++)
        s += a[i];
    return s;
}

static void *worker(void *arg)
{
    long id = (long)arg;
    double seed = 1.0 + (double)id;

    /* Establish the known-correct value once (same code, same input). */
    double golden = kernel_round(seed);

    for (int r = 0; r < ROUNDS && !g_stop; r++) {
        /* Churn affinity to force migration mid-computation. */
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET((unsigned)((id + r) & 3), &set);
        sched_setaffinity(0, sizeof(set), &set);

        double v = kernel_round(seed);
        if (memcmp(&v, &golden, sizeof(v)) != 0) {
            g_fail = 1;
            g_stop = 1;
            return NULL;
        }
        sched_yield();
    }
    return NULL;
}

/* Unrelated FP load on its own thread to keep all CPUs busy and migrating. */
static void *noise(void *arg)
{
    (void)arg;
    volatile double x = 3.14159;
    while (!g_stop) {
        for (int i = 0; i < 100000; i++)
            x = x * 1.0000003 + 0.0001 - (x * 0.0000003);
    }
    return NULL;
}

int main(void)
{
    pthread_t w[NWORK], n[4];

    for (int i = 0; i < 4; i++)
        pthread_create(&n[i], NULL, noise, NULL);
    for (long i = 0; i < NWORK; i++)
        pthread_create(&w[i], NULL, worker, (void *)i);

    for (int i = 0; i < NWORK; i++)
        pthread_join(w[i], NULL);
    g_stop = 1;
    for (int i = 0; i < 4; i++)
        pthread_join(n[i], NULL);

    if (g_fail) {
        write(1, "FAIL:fp-mismatch\n", 17);
        return 1;
    }
    write(1, "OK\n", 3);
    return 0;
}
