#include "pit.h"
#include <asm/io.h>
#include <kernel/driver.h>
#include <kernel/kprintf.h>
#include <kernel/tick.h>

#define PIT_CH0   0x40
#define PIT_CMD   0x43
#define PIT_FREQ  1193182UL

static volatile uint64_t ticks;

void pit_handler(void)
{
	ticks++;
	tick_notify();
}

void pit_init(uint32_t hz)
{
	uint16_t div = (uint16_t)(PIT_FREQ / hz);
	outb(PIT_CMD, 0x36);          /* channel 0, lo/hi, square wave */
	outb(PIT_CH0, div & 0xFF);
	outb(PIT_CH0, div >> 8);
	kprintf("PIT timer: %d Hz\n", (int)hz);
}

static driver_t pit_driver = {
	.name        = "pit",
	.type        = DRIVER_TYPE_TIMER,
	.initialized = 1,
};

uint64_t timer_ticks(void) { return ticks; }

void pit_register(void)
{
	driver_register(&pit_driver);
}
