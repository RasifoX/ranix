#include <drivers/irqchip/arm_timer.h>
#include <kernel/board.h>
#include <kernel/kprintf.h>
#include <kernel/driver.h>
#include <kernel/sched.h>

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

    /* wake sleeping tasks so they can check their conditions */
    for (int i = 0; i < task_count(); i++) {
        task_t *t = task_get(i);
        if (t && t->state == TASK_SLEEPING)
            task_wake(t);
    }
}

void timer_init(uint32_t hz) {
    timer_hz = hz;

    uint64_t freq, count;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(count));
    count += freq / hz;
    __asm__ volatile("msr cntv_cval_el0, %0" :: "r"(count));
    __asm__ volatile("msr cntv_ctl_el0,  %0" :: "r"((uint64_t)1));

    board_config_t *cfg = board_get_config();
    *(volatile uint32_t *)(cfg->local_base + 0x40) = (1 << 3);

    kprintf("[TIMER] %d Hz, freq=%d\n", (int)hz, (int)freq);
}

uint64_t timer_ticks(void) { return ticks; }

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