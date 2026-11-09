/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* The upper halves of ymm survive preemption, syscalls and signal handlers. */
#define _GNU_SOURCE
#ifndef __x86_64__
#include <stdio.h>
int main(void) { puts("OK no AVX here"); return 0; }
#else
#include <cpuid.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static volatile int stop, bad;

static inline void ymm_fill(unsigned v)
{
    unsigned b[8];
    for (int i = 0; i < 8; i++) b[i] = v + (unsigned)i;
    __asm__ volatile("vmovdqu %0, %%ymm3\n\tvmovdqu %0, %%ymm9" : : "m"(b) : "xmm3", "xmm9");
}

static inline void ymm_read(unsigned *a, unsigned *c)
{
    __asm__ volatile("vmovdqu %%ymm3, %0\n\tvmovdqu %%ymm9, %1" : "=m"(*(unsigned(*)[8])a), "=m"(*(unsigned(*)[8])c));
}

static void *spin(void *arg)
{
    unsigned v = (unsigned)(long)arg << 24;
    while (!stop) {
        v = (v & 0xFF000000u) | ((v + 1) & 0xFFFFFF);
        ymm_fill(v);
        syscall(SYS_getpid);
        for (volatile int i = 0; i < 2000; i++) { }
        sched_yield();
        unsigned a[8], c[8];
        ymm_read(a, c);
        for (int i = 0; i < 8; i++)
            if (a[i] != v + (unsigned)i || c[i] != v + (unsigned)i) bad = 1;
    }
    return NULL;
}

static void on_sig(int s)
{
    (void)s;
    ymm_fill(0xDEAD0000u);
}

int main(void)
{
    unsigned a, b, c, d;
    __cpuid(1, a, b, c, d);
    if (!(c & (1u << 27)) || !(c & (1u << 28))) { puts("OK no AVX on this CPU"); return 0; }
    signal(SIGUSR1, on_sig);
    pthread_t t[6];
    for (long i = 0; i < 6; i++) pthread_create(&t[i], NULL, spin, (void *)(i + 1));
    for (int i = 0; i < 300; i++) {
        for (int k = 0; k < 6; k++) pthread_kill(t[k], SIGUSR1);
        usleep(2000);
    }
    stop = 1;
    for (int i = 0; i < 6; i++) pthread_join(t[i], NULL);
    puts(bad ? "FAIL" : "OK");
    return bad;
}
#endif
