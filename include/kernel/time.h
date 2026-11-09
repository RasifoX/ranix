#ifndef KERNEL_TIME_H
#define KERNEL_TIME_H

/* Kernel time in one place. kernel_ticks counts timer interrupts since boot at
 * TIMER_HZ (board.h); mono_ns() is CLOCK_MONOTONIC in nanoseconds; the wall
 * clock is read from and written to the board's RTC. */

#include <uapi/types.h>
#include <kernel/board.h>

extern volatile uint64_t kernel_ticks;           /* kernel/sched.c */

uint64_t mono_ns(void);                          /* kernel/sys/syscall_proc.c */
void ranix_monotime(int64_t *sec, int64_t *nsec);
void ranix_walltime(int64_t *sec, int64_t *nsec);

void    rtc_init(void);                          /* read the RTC into the wall clock */
int64_t arch_rtc_read(void);                     /* board code: RTC as Unix seconds */
void    arch_rtc_write(int64_t epoch_sec);

void timerfd_tick(void);                         /* kernel/sys/syscall_event.c */

#endif
