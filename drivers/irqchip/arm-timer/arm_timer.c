/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <drivers/irqchip/arm_timer.h>
#include <kernel/board.h>
#include <kernel/kprintf.h>
#include <kernel/driver.h>
#include <kernel/tick.h>
#include <kernel/hal.h>
#include <kernel/time.h>
#include <kernel/smp.h>

static volatile uint64_t ticks = 0;
static uint32_t timer_hz = 100;

/* This CPU's tick period in counter units: TICK_SLOW times longer while the
 * scheduler has it slowed (sched_tick then adds TICK_SLOW to kernel_ticks, so
 * the hardware must really slow down, or idle time runs ten times fast). */
static uint64_t cnt_freq(void) {
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq;
}

static uint64_t tick_period(int slow) {
    return cnt_freq() / timer_hz * (slow ? TICK_SLOW : 1);
}

static inline uint64_t cnt_now(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

/* The compare register is a one-shot: it is set for whichever comes first,
 * this CPU's next tick (kept at absolute times, no drift) or its earliest fine
 * wakeup (hrtimer_next, CLOCK_MONOTONIC ns = the counter scaled). */
static uint64_t tick_due[MAX_CPUS];

static uint64_t ns_to_cnt(uint64_t ns) {   /* rounded up: never before the instant */
    uint64_t f = cnt_freq();
    return ns / 1000000000ULL * f + ((ns % 1000000000ULL) * f + 999999999ULL) / 1000000000ULL;
}

void arch_timer_reprogram(void) {
    uint64_t at = tick_due[arch_get_cpu_ctx()->cpu_id];
    uint64_t hr = hrtimer_next();
    if (hr != ~0ULL) { uint64_t h = ns_to_cnt(hr); if (h < at) at = h; }
    __asm__ volatile("msr cntv_cval_el0, %0; isb" :: "r"(at));
}

void timer_handler(void) {
    ticks++;
    cpu_context_t *c = arch_get_cpu_ctx();
    uint64_t now = cnt_now(), *due = &tick_due[c->cpu_id];
    uint64_t tol = cnt_freq() / 50000;   /* 20 us: a few us early is the tick */
    if (now + tol >= *due) {
        *due += tick_period(c->tick_slow);
        if (*due <= now) *due = now + tick_period(c->tick_slow);   /* fell behind: no burst */
        /* Only the BSP advances global time + runs the global walk (kernel_ticks,
         * sleeper wakeups, itimers). Secondary cores do scheduler accounting only —
         * otherwise every core bumps kernel_ticks and wall-clock runs N× fast on an
         * N-core SMP (the printk timestamps were ~4-5x real on the Pi). Mirrors x86. */
        if (c->cpu_id == 0) tick_notify();
        else                tick_notify_local();
    }
    hrtimer_run(mono_ns());
    arch_timer_reprogram();
}

/* Allow EL0 to read the virtual + physical counters (CNTKCTL_EL1.EL0VCTEN|
 * EL0PCTEN). Per-CPU register — set on every core. Enables fast userspace
 * timing (vDSO-style clock_gettime) without a syscall trap. */
static void timer_enable_el0_counter(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntkctl_el1" : "=r"(v));
    v |= (1u << 0) | (1u << 1);
    __asm__ volatile("msr cntkctl_el1, %0; isb" :: "r"(v));
}

void timer_init(uint32_t hz) {
    timer_hz = hz;
    timer_enable_el0_counter();

    tick_due[arch_get_cpu_ctx()->cpu_id] = cnt_now() + tick_period(0);
    hrtimer_on = 1;
    arch_timer_reprogram();
    __asm__ volatile("msr cntv_ctl_el0,  %0" :: "r"((uint64_t)1));

    board_config_t *cfg = board_get_config();
    *(volatile uint32_t *)(cfg->local_base + 0x40) = (1 << 3);

    kprintf(KERN_INFO "timer: arm %d Hz\n", (int)hz);
}

uint64_t timer_ticks(void) { return ticks; }

/* this CPU's timer: TICK_SLOW times slower, or back to normal at once */
void arch_tick_slow(int slow) {
    tick_due[arch_get_cpu_ctx()->cpu_id] = cnt_now() + tick_period(slow);
    arch_timer_reprogram();
}

void timer_secondary_init(void) {
    timer_enable_el0_counter();
    tick_due[arch_get_cpu_ctx()->cpu_id] = cnt_now() + tick_period(0);
    arch_timer_reprogram();
    __asm__ volatile("msr cntv_ctl_el0,  %0" :: "r"((uint64_t)1));
}

static driver_t timer_driver = {
    .name = "arm-timer",
    .type = DRIVER_TYPE_TIMER,
    .init = 0,
    .read = 0,
    .write = 0,
    .ioctl = 0,
    .initialized = 1,
};

void timer_register(void)
{
    driver_register(&timer_driver);
}