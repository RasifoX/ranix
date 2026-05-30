#include <drivers/irqchip/arm_timer.h>
#include <kernel/board.h>
#include <kernel/kprintf.h>
#include <kernel/driver.h>
#include <kernel/tick.h>

static volatile uint64_t ticks = 0;
static uint32_t timer_hz = 100;

void timer_handler(void) {
    ticks++;

    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t next;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(next));
    next += freq / timer_hz;
    __asm__ volatile("msr cntv_cval_el0, %0" :: "r"(next));

    tick_notify();
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

    uint64_t freq, count;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(count));
    count += freq / hz;
    __asm__ volatile("msr cntv_cval_el0, %0" :: "r"(count));
    __asm__ volatile("msr cntv_ctl_el0,  %0" :: "r"((uint64_t)1));

    board_config_t *cfg = board_get_config();
    *(volatile uint32_t *)(cfg->local_base + 0x40) = (1 << 3);

    kprintf("ARM timer: %d Hz\n", (int)hz);
}

uint64_t timer_ticks(void) { return ticks; }

void timer_secondary_init(void) {
    timer_enable_el0_counter();
    uint64_t freq, count;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(count));
    count += freq / timer_hz;
    __asm__ volatile("msr cntv_cval_el0, %0" :: "r"(count));
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